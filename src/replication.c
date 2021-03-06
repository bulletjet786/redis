/* Asynchronous replication implementation.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include "server.h"

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>

void replicationDiscardCachedMaster(void);
void replicationResurrectCachedMaster(int newfd);
void replicationSendAck(void);
void putSlaveOnline(client *slave);
int cancelReplicationHandshake(void);

/* --------------------------- Utility functions ---------------------------- */

/* Return the pointer to a string representing the slave ip:listening_port
 * pair. Mostly useful for logging, since we want to log a slave using its
 * IP address and its listening port which is more clear for the user, for
 * example: "Closing connection with replica 10.1.2.3:6380". */
char *replicationGetSlaveName(client *c) {
    static char buf[NET_PEER_ID_LEN];
    char ip[NET_IP_STR_LEN];

    ip[0] = '\0';
    buf[0] = '\0';
    if (c->slave_ip[0] != '\0' ||
        anetPeerToString(c->fd,ip,sizeof(ip),NULL) != -1)
    {
        /* Note that the 'ip' buffer is always larger than 'c->slave_ip' */
        if (c->slave_ip[0] != '\0') memcpy(ip,c->slave_ip,sizeof(c->slave_ip));

        if (c->slave_listening_port)
            anetFormatAddr(buf,sizeof(buf),ip,c->slave_listening_port);
        else
            snprintf(buf,sizeof(buf),"%s:<unknown-replica-port>",ip);
    } else {
        snprintf(buf,sizeof(buf),"client id #%llu",
            (unsigned long long) c->id);
    }
    return buf;
}

/* ---------------------------------- MASTER -------------------------------- */

void createReplicationBacklog(void) {
    serverAssert(server.repl_backlog == NULL);
    server.repl_backlog = zmalloc(server.repl_backlog_size);
    server.repl_backlog_histlen = 0;
    server.repl_backlog_idx = 0;

    /* We don't have any data inside our buffer, but virtually the first
     * byte we have is the next byte that will be generated for the
     * replication stream. */
    server.repl_backlog_off = server.master_repl_offset+1;
}

/* This function is called when the user modifies the replication backlog
 * size at runtime. It is up to the function to both update the
 * server.repl_backlog_size and to resize the buffer and setup it so that
 * it contains the same data as the previous one (possibly less data, but
 * the most recent bytes, or the same data and more free space in case the
 * buffer is enlarged). */
void resizeReplicationBacklog(long long newsize) {
    if (newsize < CONFIG_REPL_BACKLOG_MIN_SIZE)
        newsize = CONFIG_REPL_BACKLOG_MIN_SIZE;
    if (server.repl_backlog_size == newsize) return;

    server.repl_backlog_size = newsize;
    if (server.repl_backlog != NULL) {
        /* What we actually do is to flush the old buffer and realloc a new
         * empty one. It will refill with new data incrementally.
         * The reason is that copying a few gigabytes adds latency and even
         * worse often we need to alloc additional space before freeing the
         * old buffer. */
        zfree(server.repl_backlog);
        server.repl_backlog = zmalloc(server.repl_backlog_size);
        server.repl_backlog_histlen = 0;
        server.repl_backlog_idx = 0;
        /* Next byte we have is... the next since the buffer is empty. */
        server.repl_backlog_off = server.master_repl_offset+1;
    }
}

void freeReplicationBacklog(void) {
    serverAssert(listLength(server.slaves) == 0);
    zfree(server.repl_backlog);
    server.repl_backlog = NULL;
}

/* Add data to the replication backlog.
 * This function also increments the global replication offset stored at
 * server.master_repl_offset, because there is no case where we want to feed
 * the backlog without incrementing the offset. */
void feedReplicationBacklog(void *ptr, size_t len) {
    unsigned char *p = ptr;

    server.master_repl_offset += len;

    /* This is a circular buffer, so write as much data we can at every
     * iteration and rewind the "idx" index if we reach the limit. */
    while(len) {
        size_t thislen = server.repl_backlog_size - server.repl_backlog_idx;
        if (thislen > len) thislen = len;
        memcpy(server.repl_backlog+server.repl_backlog_idx,p,thislen);
        server.repl_backlog_idx += thislen;
        if (server.repl_backlog_idx == server.repl_backlog_size)
            server.repl_backlog_idx = 0;
        len -= thislen;
        p += thislen;
        server.repl_backlog_histlen += thislen;
    }
    if (server.repl_backlog_histlen > server.repl_backlog_size)
        server.repl_backlog_histlen = server.repl_backlog_size;
    /* Set the offset of the first byte we have in the backlog. */
    server.repl_backlog_off = server.master_repl_offset -
                              server.repl_backlog_histlen + 1;
}

/* Wrapper for feedReplicationBacklog() that takes Redis string objects
 * as input. */
void feedReplicationBacklogWithObject(robj *o) {
    char llstr[LONG_STR_SIZE];
    void *p;
    size_t len;

    if (o->encoding == OBJ_ENCODING_INT) {
        len = ll2string(llstr,sizeof(llstr),(long)o->ptr);
        p = llstr;
    } else {
        len = sdslen(o->ptr);
        p = o->ptr;
    }
    feedReplicationBacklog(p,len);
}

/* 传播写命令到slaves，并且填充复制积压。
 * 当这个实例是master时，我们使用这个函数来接收clients的命令，来创建复制流。
 * 如果当前实例是slave，并且有sub-slaves，我们将会使用replicationFeedSlavesFromMaster函数。 */
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j, len;
    char llstr[LONG_STR_SIZE];

    /* 仅当该实例是顶层master时，才进行主从复制。
     * 当该实例是slave时，我们将直接代理复制流，来保证复制流的完全一致。
     * 因为所有的slaves将会共享同一个复制流，它们应该有相同的复制历史和偏移量。 */
    if (server.masterhost != NULL) return;

    /* 如果没有slave，且没有复制积压缓冲区要填充，就直接返回 */
    if (server.repl_backlog == NULL && listLength(slaves) == 0) return;

    serverAssert(!(listLength(slaves) != 0 && server.repl_backlog == NULL));

    /* 我们需要先对每一个slave发送SELECT命令以切换到正确的数据库 */
    if (server.slaveseldb != dictid) {
        robj *selectcmd;

        /* For a few DBs we have pre-computed SELECT command. */
        if (dictid >= 0 && dictid < PROTO_SHARED_SELECT_CMDS) {
            selectcmd = shared.select[dictid];
        } else {
            int dictid_len;

            dictid_len = ll2string(llstr,sizeof(llstr),dictid);
            selectcmd = createObject(OBJ_STRING,
                sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, llstr));
        }

        /* 增加SELECT命令到复制积压缓冲区 */
        if (server.repl_backlog) feedReplicationBacklogWithObject(selectcmd);

        /* 把命令发送给slave */
        listRewind(slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            /* master正准备生成RDB文件，这条命令将会被包含进RDB中，不发送 */
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;
            addReply(slave,selectcmd);
        }

        if (dictid < 0 || dictid >= PROTO_SHARED_SELECT_CMDS)
            decrRefCount(selectcmd);
    }
    server.slaveseldb = dictid;

    /* 将命令以RESP的格式写入积压缓冲区中 */
    if (server.repl_backlog) {
        char aux[LONG_STR_SIZE+3];

        aux[0] = '*';
        len = ll2string(aux+1,sizeof(aux)-1,argc);
        aux[len+1] = '\r';
        aux[len+2] = '\n';
        feedReplicationBacklog(aux,len+3);

        for (j = 0; j < argc; j++) {
            long objlen = stringObjectLen(argv[j]);

            aux[0] = '$';
            len = ll2string(aux+1,sizeof(aux)-1,objlen);
            aux[len+1] = '\r';
            aux[len+2] = '\n';
            feedReplicationBacklog(aux,len+3);
            feedReplicationBacklogWithObject(argv[j]);
            feedReplicationBacklog(aux+len+1,2);
        }
    }

    /* 将命令写入到每个slaves */
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        /* master正在生成RDB文件，这条命令将会被包含进RDB中，不发送 */
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;

        /* slaves可能正在进行SYNC，我们先添加到client的output buffer中。
         * 当SYNC完成后将会发送给slaves */

        addReplyMultiBulkLen(slave,argc);

        for (j = 0; j < argc; j++)
            addReplyBulk(slave,argv[j]);
    }
}

/* 这个函数用来实现代理：将我们从master客户端节点接收到的命令复制到子slave
 * 为什么slave-slave间的复制不复用replicationFeedSlaves：
 * 1) 在master->slave复制时会增加SELECT命令，slave->sub-slave没必要重复添加
 * 2) 可以保证slave和sub-slave同master完全一致!。
 * */
#include <ctype.h>
void replicationFeedSlavesFromMasterStream(list *slaves, char *buf, size_t buflen) {
    listNode *ln;
    listIter li;

    /* Debugging: this is handy to see the stream sent from master
     * to slaves. Disabled with if(0). */
    if (0) {
        printf("%zu:",buflen);
        for (size_t j = 0; j < buflen; j++) {
            printf("%c", isprint(buf[j]) ? buf[j] : '.');
        }
        printf("\n");
    }

    if (server.repl_backlog) feedReplicationBacklog(buf,buflen);
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        /* 对于还在还未开始RDB生成的slaves，我们添加到输出缓冲区 */
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;
        addReplyString(slave,buf,buflen);
    }
}

/* 向监视器发送监视命令信息：
 * protocol:    unix_stamp [database_id client] full_cmd
 * example:     1378822099.421623 [0 127.0.0.1:56604] "SET" "msg" "hello world"
 * */
void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;

    /* 写unix_stamp,database_id,client到cmdrepr中 */
    gettimeofday(&tv,NULL);
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%06ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    if (c->flags & CLIENT_LUA) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d lua] ",dictid);
    } else if (c->flags & CLIENT_UNIX_SOCKET) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d unix:%s] ",dictid,server.unixsocket);
    } else {
        cmdrepr = sdscatprintf(cmdrepr,"[%d %s] ",dictid,getClientPeerId(c));
    }

    /* 向cmdrepr中写入命令 */
    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == OBJ_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "\"%ld\"", (long)argv[j]->ptr);
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)argv[j]->ptr,
                        sdslen(argv[j]->ptr));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    /* 创建监视消息对象 */
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    cmdobj = createObject(OBJ_STRING,cmdrepr);

    /* 遍历监视器，发送监视消息 */
    listRewind(monitors,&li);
    while((ln = listNext(&li))) {
        client *monitor = ln->value;
        addReply(monitor,cmdobj);
    }
    decrRefCount(cmdobj);
}

/* 从数据缓冲区中发送指定offset的数据，直到末尾 */
long long addReplyReplicationBacklog(client *c, long long offset) {
    long long j, skip, len;

    serverLog(LL_DEBUG, "[PSYNC] Replica request offset: %lld", offset);

    if (server.repl_backlog_histlen == 0) {
        serverLog(LL_DEBUG, "[PSYNC] Backlog history len is zero");
        return 0;
    }

    serverLog(LL_DEBUG, "[PSYNC] Backlog size: %lld",
             server.repl_backlog_size);
    serverLog(LL_DEBUG, "[PSYNC] First byte: %lld",
             server.repl_backlog_off);
    serverLog(LL_DEBUG, "[PSYNC] History len: %lld",
             server.repl_backlog_histlen);
    serverLog(LL_DEBUG, "[PSYNC] Current index: %lld",
             server.repl_backlog_idx);

    /* 计算我们需要丢弃的字节量 */
    skip = offset - server.repl_backlog_off;
    serverLog(LL_DEBUG, "[PSYNC] Skipping: %lld", skip);

    /* 计算获取复制缓冲区的首个字节所在位置 */
    j = (server.repl_backlog_idx +
        (server.repl_backlog_size-server.repl_backlog_histlen)) %
        server.repl_backlog_size;
    serverLog(LL_DEBUG, "[PSYNC] Index of first byte: %lld", j);

    /* 定位到指定的offset */
    j = (j + skip) % server.repl_backlog_size;

    /* 发送数据到slave。因为是个环形缓冲区，如果我们触到了边界，
     * 我们不得不把响应切分成两部分 */
    len = server.repl_backlog_histlen - skip;
    serverLog(LL_DEBUG, "[PSYNC] Reply total length: %lld", len);
    while(len) {
        long long thislen =
            ((server.repl_backlog_size - j) < len) ?
            (server.repl_backlog_size - j) : len;

        serverLog(LL_DEBUG, "[PSYNC] addReply() length: %lld", thislen);
        addReplySds(c,sdsnewlen(server.repl_backlog + j, thislen));
        len -= thislen;
        j = 0;
    }
    return server.repl_backlog_histlen - skip;
}

/* 返回偏移量。这个返回值仅仅在BGSAVE启动后且clients未执行任何命令时有效 */
long long getPsyncInitialOffset(void) {
    return server.master_repl_offset;
}

/* 在完整重同步时发送一个FULLRESYNC的回复，根据不同的清空，产生不同的副作用：
 * 1) 记住，在client中，我们把offset记录到psync_initial_offset，来保证后面slave可以
 *  附加到这个BGSAVE进程，通过获取这个offset和复制client的output。
 * 2) 设置WAIT_BGSAVE_END，这样我们可以积累增量数据。
 * 3) 强制复制流发射一个SELECT命令到新的slave来选择正确的数据库。
 * 正常情况下应该在这两种情况下调用：
 * 1) 在成功启动BGSAVE后调用
 * 2) 当已经有一个BGSAVE在执行时，另一个slave附加在这个BGSAVE上
 * */
int replicationSetupSlaveForFullResync(client *slave, long long offset) {
    char buf[128];
    int buflen;

    slave->psync_initial_offset = offset;
    slave->replstate = SLAVE_STATE_WAIT_BGSAVE_END;
    /* 我们将会开始记录增量数据，通过设置slaveseldb=-1来强制发射SELECT语句。 */
    server.slaveseldb = -1;

    /* 如果是SYNV命令，则不发送+FULLRESYNC回复 */
    if (!(slave->flags & CLIENT_PRE_PSYNC)) {
        buflen = snprintf(buf,sizeof(buf),"+FULLRESYNC %s %lld\r\n",
                          server.replid,offset);
        if (write(slave->fd,buf,buflen) != buflen) {
            freeClientAsync(slave);
            return C_ERR;
        }
    }
    return C_OK;
}

/* 这个函数从master的视角处理PSYNC请求。
 * 可以进行部分重同步返回C_OK，需要进行完整重同步返回C_ERR。*/
int masterTryPartialResynchronization(client *c) {
    long long psync_offset, psync_len;
    char *master_replid = c->argv[1]->ptr;
    char buf[128];
    int buflen;

    /* 解析slave指定的复制偏移量。
     * 如果解析错误，则进行完整重同步：这一般不会发生，但是处理这种情况可以提高鲁棒性。 */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&psync_offset,NULL) !=
       C_OK) goto need_full_resync;

    /* server的replid是否和slave宣告的一致。
     * 如果replid改变了，这个master将会有一个不同的复制历史，就不能进行完整重同步。
     * 注意：那里有两个潜在的合法replid。然而id2是否合法取决于指定的偏移量。
     * 如果replid和server.replid2相同，说明master和slave曾经都作为slave同步过，
     * 后来进行了故障转移，则仅当slave没有该新的slave复制的多时，才允许部分重同步。
     * */
    if (strcasecmp(master_replid, server.replid) &&
        (strcasecmp(master_replid, server.replid2) ||
         psync_offset > server.second_replid_offset))
    {
        /* replid=?意味这slave要求进行完整重同步 */
        if (master_replid[0] != '?') {
            if (strcasecmp(master_replid, server.replid) &&
                strcasecmp(master_replid, server.replid2))
            {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Replication ID mismatch (Replica asked for '%s', my "
                    "replication IDs are '%s' and '%s')",
                    master_replid, server.replid, server.replid2);
            } else {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Requested offset for second ID was %lld, but I can reply "
                    "up to %lld", psync_offset, server.second_replid_offset);
            }
        } else {
            serverLog(LL_NOTICE,"Full resync requested by replica %s",
                replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* 检查我们是否有slave需要的数据 */
    if (!server.repl_backlog ||
        psync_offset < server.repl_backlog_off ||
        psync_offset > (server.repl_backlog_off + server.repl_backlog_histlen))
    {
        serverLog(LL_NOTICE,
            "Unable to partial resync with replica %s for lack of backlog (Replica request was: %lld).", replicationGetSlaveName(c), psync_offset);
        if (psync_offset > server.master_repl_offset) {
            serverLog(LL_WARNING,
                "Warning: replica %s tried to PSYNC with an offset that is greater than the master replication offset.", replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* 如果到达了这里，说明我们可以执行一个部分重同步：
     * 1) 设置当前的client为slave。
     * 2) 使用+CONTINUE通告客户端我们可以进行部分重同步。
     * 3) 发送复制缓冲区中的数据。
     * */
    c->flags |= CLIENT_SLAVE;
    c->replstate = SLAVE_STATE_ONLINE;
    c->repl_ack_time = server.unixtime;
    c->repl_put_online_on_ack = 0;
    listAddNodeTail(server.slaves,c);
    /* 在这个阶段，我们不使用client的输出缓冲区时为了加速新的命令。
     * 但是，此时我们确定这个socket的发送缓冲区是空的，因此我们的写操作实际上不会错误。 */
    if (c->slave_capa & SLAVE_CAPA_PSYNC2) {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE %s\r\n", server.replid);
    } else {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
    }
    if (write(c->fd,buf,buflen) != buflen) {        // 出现了短写，断开重来
        freeClientAsync(c);
        return C_OK;
    }
    // 发送复制缓冲区中积累的数据
    psync_len = addReplyReplicationBacklog(c,psync_offset);
    serverLog(LL_NOTICE,
        "Partial resynchronization request from %s accepted. Sending %lld bytes of backlog starting from offset %lld.",
            replicationGetSlaveName(c),
            psync_len, psync_offset);
    /* 注意：我们不需要设置server.slaveseldb为-1来强制master发射SELECT，因为这个slave
     * 从先前到master的连接中获取了它自己的状态 */

    refreshGoodSlavesCount();       // 刷新存活的slave的数量
    return C_OK; /* 返回OK，表示进行部分重同步*/

need_full_resync:
    /* 我们需要一个完整重同步。
     * 注意：我们不能理解回复给slave一个PSYNC。PSYNC的回复里需要包含生成RDB文件时master的offset，
     * 因此我们需要延迟回复。*/
    return C_ERR;
}

/* 为了复制开启一个BGSAVE进程，根据配置选择disk或者socket，并且确保在开始前脚本缓存被清理。
 * 入参mincapa是一个slave能力的位表示，可以通过SLAVE_CAPA_*相关宏测试。
 * 副作用：
 * 1) 如果可能开始一个RDB，则处理WAIT_START状态，否则发送一个错误，并从slaves列表中移除。
 * 2) 如果BGSAVE开始清空Lua脚本缓存
 * 成功返回OK，失败返回ERR。
 * */
int startBgsaveForReplication(int mincapa) {
    int retval;
    int socket_target = server.repl_diskless_sync && (mincapa & SLAVE_CAPA_EOF);
    listIter li;
    listNode *ln;

    serverLog(LL_NOTICE,"Starting BGSAVE for SYNC with target: %s",
        socket_target ? "replicas sockets" : "disk");

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    /* 仅当rsipt不为NULL时才做rdbSave，否则slave将会错过这次复制流 */
    if (rsiptr) {
        if (socket_target)
            retval = rdbSaveToSlavesSockets(rsiptr);
        else
            retval = rdbSaveBackground(server.rdb_filename,rsiptr);
    } else {
        serverLog(LL_WARNING,"BGSAVE for replication: replication information not available, can't generate the RDB file right now. Try later.");
        retval = C_ERR;
    }

    /* 如果我们BGSAVE失败，需要从slaves中删除等待完整重同步的slave，并提示错误，异步关闭连接 */
    if (retval == C_ERR) {
        serverLog(LL_WARNING,"BGSAVE for replication failed");
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                slave->flags &= ~CLIENT_SLAVE;
                listDelNode(server.slaves,ln);
                addReplyError(slave,
                    "BGSAVE failed, replication can't continue");
                slave->flags |= CLIENT_CLOSE_AFTER_REPLY;
            }
        }
        return retval;
    }

    /* 如果target=socket, rdbSaveToSlavesSockets()将会设置slaves变更为全同步，
     * 对于target=disk, 我们将会现在做这些。 */
    if (!socket_target) {
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                    replicationSetupSlaveForFullResync(slave,
                            getPsyncInitialOffset());
            }
        }
    }

    /* Flush the script cache, since we need that slave differences are
     * accumulated without requiring slaves to match our cached scripts. */
    if (retval == C_OK) replicationScriptCacheFlush();
    return retval;
}

/* SYNC和PSYNC命令的实现 */
void syncCommand(client *c) {
    /* 如果客户端已经是一个slave或者monitor，则直接返回 */
    if (c->flags & CLIENT_SLAVE) return;

    /* 如果我们是一个slave但是还没有跟随成功我们的master，则拒绝 */
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED) {
        addReplySds(c,sdsnew("-NOMASTERLINK Can't SYNC while not connected with my master\r\n"));
        return;
    }

    /* 当执行同步时，client的回复缓冲区必须是空的。我们在执行BGSAVE时，可能复用RDB文件，
     * 会在两个slave之间拷贝回复缓冲区 */
    if (clientHasPendingReplies(c)) {
        addReplyError(c,"SYNC and PSYNC are invalid with pending output");
        return;
    }

    serverLog(LL_NOTICE,"Replica %s asks for synchronization",
        replicationGetSlaveName(c));

    /* 如果是PSYNC命令，尝试进行部分重同步。
     * 如果失败了，我们将会进行一个完整重同步，通过返回：
     * +FULLRESYNC <replid> <offset>
     * 这样slave可以在断开到master的连接时，知道新的replid和偏移量，来进行一个部分重同步的重连。
     * */
    if (!strcasecmp(c->argv[0]->ptr,"psync")) {
        if (masterTryPartialResynchronization(c) == C_OK) {
            server.stat_sync_partial_ok++;
            return; /* 不需要完整重同步，直接返回 */
        } else {
            char *master_replid = c->argv[1]->ptr;

            /* 仅当slave被迫执行部分重同步出错时，才更新stat_sync_partial_err */
            if (master_replid[0] != '?') server.stat_sync_partial_err++;
        }
    } else {
        /* 如果是SYNC命令，我们将会使用复制协议的一个老的实现。标记客户端，我们不希望接受
         * REPLCONF ACK的反馈信息。 */
        c->flags |= CLIENT_PRE_PSYNC;
    }

    /* 下面进行完整重同步 */
    server.stat_sync_full++;

    /* 设置slave复制状态为SLAVE_STATE_WAIT_BGSAVE_START。下面的代码路径将会根据
     * 我们不同的处理来修改状态。 */
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    if (server.repl_disable_tcp_nodelay)    // 如果禁用TCP NoDelay选项
        anetDisableTcpNoDelay(NULL, c->fd); /* 失败了也不要紧 */
    c->repldbfd = -1;
    c->flags |= CLIENT_SLAVE;
    listAddNodeTail(server.slaves,c);

    /* 如果需要，我们创建repl_backlog缓冲区 */
    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL) {
        /* 当我们创建backlog时，我们总是使用新的replid并且清理id2，
         * 这样就不会有非法的历史数据了 */
        changeReplicationId();
        clearReplicationId2();
        createReplicationBacklog();
    }

    /* 第一种情况：BGSAVE正在后台进行，并且target=disk */
    if (server.rdb_child_pid != -1 &&
        server.rdb_child_type == RDB_CHILD_TYPE_DISK)
    {
        /* 现在后台有一个BGSAVE在运行。我们检查是否可以用于复制。
         * 如果有另外一个slave触发了BGSAVE，我们可以尝试复用RDB文件 */
        client *slave;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) break;
        }
        /* 如果我们的slave的能力兼容这个slave的能力，则可以复用同一个RDB文件 */
        if (ln && ((c->slave_capa & slave->slave_capa) == slave->slave_capa)) {
            /* 完成，我们和另外一个slave兼容，设置正确的状态，并copy缓冲区 */
            copyClientOutputBuffer(c,slave);
            replicationSetupSlaveForFullResync(c,slave->psync_initial_offset);
            serverLog(LL_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            /* 不兼容，我们需要等待下一个BGSAVE */
            serverLog(LL_NOTICE,"Can't attach the replica to the current BGSAVE. Waiting for next BGSAVE for SYNC");
        }

    /* 第二种情况：BGSAVE在后台运行，但是target=socket */
    } else if (server.rdb_child_pid != -1 &&
               server.rdb_child_type == RDB_CHILD_TYPE_SOCKET)
    {
        /* 又一个RDB进程正直写入socket。我们需要等待下一次BGSAVE来执行同步。 */
        serverLog(LL_NOTICE,"Current BGSAVE has socket target. Waiting for next BGSAVE for SYNC");

    /* 没有BGSAVE在后台运行 */
    } else {
        if (server.repl_diskless_sync && (c->slave_capa & SLAVE_CAPA_EOF)) {
            /* diskless复制RDB进程将会在replicationCron()函数中创建，
             * 因为我们想延迟它一段时间来等待更多的slaves。 */
            if (server.repl_diskless_sync_delay)
                serverLog(LL_NOTICE,"Delay next BGSAVE for diskless SYNC");
        } else {
            /* 如果target=disk，或者slave无法支持diskless复制，并且我们还没有RDB进程，开始一个。 */
            if (server.aof_child_pid == -1) {       // 如果没有aof重写进程，则开启
                startBgsaveForReplication(c->slave_capa);
            } else {                // 如果有aof重写进程，则延迟复制
                serverLog(LL_NOTICE,
                    "No BGSAVE in progress, but an AOF rewrite is active. "
                    "BGSAVE for replication delayed");
            }
        }
    }
    return;
}

/* REPLCONF <option> <value> <option> <value> ...
 * 这个命令用于在SYNC命令之前配置复制进程。
 * 当前这个命令用于和master通信，
 * 1) 用来告诉master端口号等相关信息，使得master的info输出中可以正确的输出相关信息。
 * 2) 也用来告诉master复制过程中的相关配置信息如CAPA来让master决策。
 * */
void replconfCommand(client *c) {
    int j;

    if ((c->argc % 2) == 0) {
        /* Number of arguments must be odd to make sure that every
         * option has a corresponding value. */
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Process every option-value pair. */
    for (j = 1; j < c->argc; j+=2) {
        if (!strcasecmp(c->argv[j]->ptr,"listening-port")) {
            long port;

            if ((getLongFromObjectOrReply(c,c->argv[j+1],
                    &port,NULL) != C_OK))
                return;
            c->slave_listening_port = port;
        } else if (!strcasecmp(c->argv[j]->ptr,"ip-address")) {
            sds ip = c->argv[j+1]->ptr;
            if (sdslen(ip) < sizeof(c->slave_ip)) {
                memcpy(c->slave_ip,ip,sdslen(ip)+1);
            } else {
                addReplyErrorFormat(c,"REPLCONF ip-address provided by "
                    "replica instance is too long: %zd bytes", sdslen(ip));
                return;
            }
        } else if (!strcasecmp(c->argv[j]->ptr,"capa")) {
            /* Ignore capabilities not understood by this master. */
            if (!strcasecmp(c->argv[j+1]->ptr,"eof"))
                c->slave_capa |= SLAVE_CAPA_EOF;
            else if (!strcasecmp(c->argv[j+1]->ptr,"psync2"))
                c->slave_capa |= SLAVE_CAPA_PSYNC2;
        } else if (!strcasecmp(c->argv[j]->ptr,"ack")) {
            /* REPLCONF ACK用来向master报告复制偏移量。这个内部命令仅仅被用于slave。 */
            long long offset;

            if (!(c->flags & CLIENT_SLAVE)) return;
            if ((getLongLongFromObject(c->argv[j+1], &offset) != C_OK))
                return;
            if (offset > c->repl_ack_off)
                c->repl_ack_off = offset;
            c->repl_ack_time = server.unixtime;
            /* 如果是无盘复制，当接收到第一个ACK时，我们需要让slave真正在线 */
            if (c->repl_put_online_on_ack && c->replstate == SLAVE_STATE_ONLINE)
                putSlaveOnline(c);
            /* 这个命令什么都不会返回 */
            return;
        } else if (!strcasecmp(c->argv[j]->ptr,"getack")) {
            /* REPLCONF GETACK is used in order to request an ACK ASAP
             * to the slave. */
            if (server.masterhost && server.master) replicationSendAck();
            return;
        } else {
            addReplyErrorFormat(c,"Unrecognized REPLCONF option: %s",
                (char*)c->argv[j]->ptr);
            return;
        }
    }
    addReply(c,shared.ok);
}

/* 这个函数将会把一个slave变更为state状态，调用的时机应该仅在：
 * 一个slave收到了RDB文件后调用，并且已经准备好发送增量数据。
 * 它会做以下事情：
 * 1) 设置为ONLINE状态(当state=ONLINE但是repl_put_online_on_ack=1时无用)
 * 2) 确保可写事件是重新安装的，从而我们可以把output缓冲区发送给slave。
 * 3) 更新good slaves数量。
 * */
void putSlaveOnline(client *slave) {
    slave->replstate = SLAVE_STATE_ONLINE;
    slave->repl_put_online_on_ack = 0;
    slave->repl_ack_time = server.unixtime; /* Prevent false timeout. */
    if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE,
        sendReplyToClient, slave) == AE_ERR) {
        serverLog(LL_WARNING,"Unable to register writable event for replica bulk transfer: %s", strerror(errno));
        freeClient(slave);
        return;
    }
    refreshGoodSlavesCount();
    serverLog(LL_NOTICE,"Synchronization with replica %s succeeded",
        replicationGetSlaveName(slave));
}

void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
    client *slave = privdata;
    UNUSED(el);
    UNUSED(mask);
    char buf[PROTO_IOBUF_LEN];
    ssize_t nwritten, buflen;

    /* 在发送RDB之前，我们需要先发送复制进程配置的序言。现在序言仅仅只是RDB文件的长度，
     * 形式是"$<length>\r\n" */
    if (slave->replpreamble) {
        nwritten = write(fd,slave->replpreamble,sdslen(slave->replpreamble));
        if (nwritten == -1) {
            serverLog(LL_VERBOSE,"Write error sending RDB preamble to replica: %s",
                strerror(errno));
            freeClient(slave);
            return;
        }
        server.stat_net_output_bytes += nwritten;
        sdsrange(slave->replpreamble,nwritten,-1);
        if (sdslen(slave->replpreamble) == 0) {
            sdsfree(slave->replpreamble);
            slave->replpreamble = NULL;
            /* 接下来发送数据 */
        } else {
            return;
        }
    }

    /* 如果序言发送完成，我们接下来传输RDB文件 */
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    buflen = read(slave->repldbfd,buf,PROTO_IOBUF_LEN);
    if (buflen <= 0) {
        serverLog(LL_WARNING,"Read error sending DB to replica: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    if ((nwritten = write(fd,buf,buflen)) == -1) {
        if (errno != EAGAIN) {
            serverLog(LL_WARNING,"Write error sending DB to replica: %s",
                strerror(errno));
            freeClient(slave);
        }
        return;
    }
    slave->repldboff += nwritten;
    server.stat_net_output_bytes += nwritten;
    // 如果发送RDB文件已经传输完成了，则删除可写事件处理器，并设置slave为在线状态
    if (slave->repldboff == slave->repldbsize) {
        close(slave->repldbfd);
        slave->repldbfd = -1;
        aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
        putSlaveOnline(slave);
    }
}

/* 这个函数在每次BGSAVE操作的末尾被调用，或者RDB传输策略切换时。
 * 这个函数的目标是当slaves等待BGSAVE操作完成时，来进行非阻塞的同步操作，
 * 并且如果slaves需要的话，调度一个新的BGSAVE操作。
 * bgsaveerr如果是OK，表示保存成功，ERR表示失败。
 * type则表示BGSAVE的target类型。
 * */
void updateSlavesWaitingBgsave(int bgsaveerr, int type) {
    listNode *ln;
    int startbgsave = 0;
    int mincapa = -1;
    listIter li;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            // 计算当前需要进行的BGSAVE的slave的capa的最小交集
            startbgsave = 1;
            mincapa = (mincapa == -1) ? slave->slave_capa :
                                        (mincapa & slave->slave_capa);
        } else if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            struct redis_stat buf;

            /* 如果是一个target=disk的BGSAVE，我们需要将RDB从磁盘发送给slave socket。
             * 否则如果是一个diskless，用于无盘复制的，我们的工作量则很少，只需要设置slave在线。 */
            if (type == RDB_CHILD_TYPE_SOCKET) {
                serverLog(LL_NOTICE,
                    "Streamed RDB transfer with replica %s succeeded (socket). Waiting for REPLCONF ACK from slave to enable streaming",
                        replicationGetSlaveName(slave));
                /* 注意：我们等待从slave过来的REPLCONF ACK消息才真正使slave在线
                 * （安装可写事件从而积累的数据可以传输）。然而我们尽快改变复制状态，
                 * 因为我们的salve现在从技术上来说已经是在线了。 */
                slave->replstate = SLAVE_STATE_ONLINE;
                slave->repl_put_online_on_ack = 1;
                slave->repl_ack_time = server.unixtime; /* Timeout otherwise. */
            } else {
                if (bgsaveerr != C_OK) {
                    freeClient(slave);
                    serverLog(LL_WARNING,"SYNC failed. BGSAVE child returned an error");
                    continue;
                }
                if ((slave->repldbfd = open(server.rdb_filename,O_RDONLY)) == -1 ||
                    redis_fstat(slave->repldbfd,&buf) == -1) {
                    freeClient(slave);
                    serverLog(LL_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                    continue;
                }
                // 初始化相关状态
                slave->repldboff = 0;
                slave->repldbsize = buf.st_size;
                slave->replstate = SLAVE_STATE_SEND_BULK;
                slave->replpreamble = sdscatprintf(sdsempty(),"$%lld\r\n",
                    (unsigned long long) slave->repldbsize);

                // 设置可写事件为sendBulkToSlave
                aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
                if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE, sendBulkToSlave, slave) == AE_ERR) {
                    freeClient(slave);
                    continue;
                }
            }
        }
    }
    // 开启一个新的BGSAVE
    if (startbgsave) startBgsaveForReplication(mincapa);
}

/* 改变当前实例的replid为一个新的随机的。这会保证当修改数据集的时候，主从之间的PSYNC的成功 */
void changeReplicationId(void) {
    getRandomHexChars(server.replid,CONFIG_RUN_ID_SIZE);
    server.replid[CONFIG_RUN_ID_SIZE] = '\0';
}

/* Clear (invalidate) the secondary replication ID. This happens, for
 * example, after a full resynchronization, when we start a new replication
 * history. */
void clearReplicationId2(void) {
    memset(server.replid2,'0',sizeof(server.replid));
    server.replid2[CONFIG_RUN_ID_SIZE] = '\0';
    server.second_replid_offset = -1;
}

/* 使用当前的replid和offset来作为次id和偏移量，并且把当前的id和offset改成新的，
 * 来开启一个新的复制历史，这应该在从服务器晋升时发生，用来支撑PSYNC2协议。 */
void shiftReplicationId(void) {
    /* 我们把second_replid_offset设置为master_repl_offset+1，是因为：
     * slave将会询问它未收到的第一个字节，因此我们需要+1。
     * 例如：作为一个slave，我们确定和master有一个50字节的复制历史，当我们晋升为master后，
     * 我们可以接受的最大偏移量是51。*/
    server.second_replid_offset = server.master_repl_offset+1;
    changeReplicationId();
    serverLog(LL_WARNING,"Setting secondary replication ID to %s, valid up to offset: %lld. New replication ID is %s", server.replid2, server.second_replid_offset, server.replid);
}

/* ----------------------------------- SLAVE -------------------------------- */

/* 如果当前复制状态为正在握手，则返回1 */
int slaveIsInHandshakeState(void) {
    return server.repl_state >= REPL_STATE_RECEIVE_PONG &&
           server.repl_state <= REPL_STATE_RECEIVE_PSYNC;
}

/* 为了防止当前master在加载RDB时slave超时，我们需要发送心跳来保活链接
 * 这个函数会在两种情况下调用：
 * 1) 当我们使用emptyDB()刷新数据库时
 * 2) 当我们从master接受一个新的RDB文件时
 * */
void replicationSendNewlineToMaster(void) {
    static time_t newline_sent;
    if (time(NULL) != newline_sent) {
        newline_sent = time(NULL);
        if (write(server.repl_transfer_s,"\n",1) == -1) {
            /* Pinging back in this stage is best-effort. */
        }
    }
}

/* Callback used by emptyDb() while flushing away old data to load
 * the new dataset received by the master. */
void replicationEmptyDbCallback(void *privdata) {
    UNUSED(privdata);
    replicationSendNewlineToMaster();
}

/* Once we have a link with the master and the synchroniziation was
 * performed, this function materializes the master client we store
 * at server.master, starting from the specified file descriptor. */
void replicationCreateMasterClient(int fd, int dbid) {
    server.master = createClient(fd);
    server.master->flags |= CLIENT_MASTER;
    server.master->authenticated = 1;
    server.master->reploff = server.master_initial_offset;
    server.master->read_reploff = server.master->reploff;
    memcpy(server.master->replid, server.master_replid,
        sizeof(server.master_replid));
    /* If master offset is set to -1, this master is old and is not
     * PSYNC capable, so we flag it accordingly. */
    if (server.master->reploff == -1)
        server.master->flags |= CLIENT_PRE_PSYNC;
    if (dbid != -1) selectDb(server.master,dbid);
}

void restartAOF() {
    int retry = 10;
    while (retry-- && startAppendOnly() == C_ERR) {
        serverLog(LL_WARNING,"Failed enabling the AOF after successful master synchronization! Trying it again in one second.");
        sleep(1);
    }
    if (!retry) {
        serverLog(LL_WARNING,"FATAL: this replica instance finished the synchronization with its master, but the AOF can't be turned on. Exiting now.");
        exit(1);
    }
}

/* 异步读取从master传递来的RDB文件 */
#define REPL_MAX_WRITTEN_BEFORE_FSYNC (1024*1024*8) /* 8 MB */
void readSyncBulkPayload(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[4096];
    ssize_t nread, readlen, nwritten;
    off_t left;
    UNUSED(el);
    UNUSED(privdata);
    UNUSED(mask);

    /* 静态变量用来保持EOF标记和最后接受的字节：当它们匹配时说明到达了文件传输的末尾 */
    static char eofmark[CONFIG_RUN_ID_SIZE];
    static char lastbytes[CONFIG_RUN_ID_SIZE];
    static int usemark = 0;

    /* 如果repl_transfer_size == -1说明我们需要读取RDB文件长度 */
    if (server.repl_transfer_size == -1) {
        if (syncReadLine(fd,buf,1024,server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,
                "I/O error reading bulk count from MASTER: %s",
                strerror(errno));
            goto error;
        }

        if (buf[0] == '-') {
            serverLog(LL_WARNING,
                "MASTER aborted replication with an error: %s",
                buf+1);
            goto error;
        } else if (buf[0] == '\0') {
            /* 这个空行仅仅是保活心跳，我们更新最后交互时间就好 */
            server.repl_transfer_lastio = server.unixtime;
            return;
        } else if (buf[0] != '$') {
            serverLog(LL_WARNING,"Bad protocol from MASTER, the first byte is not '$' (we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }

        /* 有两种传输bulk数据的格式，1用于RDB文件传输，2用于diskless传输
         * 1) $381
         * 2) $EOF:<40 bytes delimiter>
         * diskless传输中无法直接获得文件长度，只能基于分隔符标记传输。这个分隔符足够长且随机，
         * 以至于和数据内容产生冲突的可能性很小，可以忽略。
         * */
        if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= CONFIG_RUN_ID_SIZE) {  // rdb传输
            usemark = 1;
            memcpy(eofmark,buf+5,CONFIG_RUN_ID_SIZE);
            memset(lastbytes,0,CONFIG_RUN_ID_SIZE);
            /* 设置repl_transfer_size=0，防止重入该条件*/
            server.repl_transfer_size = 0;
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving streamed RDB from master");
        } else {        // diskless传输
            usemark = 0;
            server.repl_transfer_size = strtol(buf+1,NULL,10);
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving %lld bytes from master",
                (long long) server.repl_transfer_size);
        }
        return;
    }

    /* 计算要读取的长度 */
    if (usemark) {
        readlen = sizeof(buf);
    } else {
        left = server.repl_transfer_size - server.repl_transfer_read;
        readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);
    }

    nread = read(fd,buf,readlen);
    if (nread <= 0) {
        serverLog(LL_WARNING,"I/O error trying to sync with MASTER: %s",
            (nread == -1) ? strerror(errno) : "connection lost");
        cancelReplicationHandshake();
        return;
    }
    server.stat_net_input_bytes += nread;

    /* 在diskless传输中，我们需要检查EOF标记，防止把它写到文件中 */
    int eof_reached = 0;

    if (usemark) {
        /* 更新lastbytes，并检查是否匹配EOF标记 */
        if (nread >= CONFIG_RUN_ID_SIZE) {
            memcpy(lastbytes,buf+nread-CONFIG_RUN_ID_SIZE,CONFIG_RUN_ID_SIZE);
        } else {
            int rem = CONFIG_RUN_ID_SIZE-nread;
            memmove(lastbytes,lastbytes+nread,rem);
            memcpy(lastbytes+rem,buf,nread);
        }
        if (memcmp(lastbytes,eofmark,CONFIG_RUN_ID_SIZE) == 0) eof_reached = 1;
    }

    server.repl_transfer_lastio = server.unixtime;
    if ((nwritten = write(server.repl_transfer_fd,buf,nread)) != nread) {
        serverLog(LL_WARNING,"Write error or short write writing to the DB dump file needed for MASTER <-> REPLICA synchronization: %s", 
            (nwritten == -1) ? strerror(errno) : "short write");
        goto error;
    }
    server.repl_transfer_read += nread;

    /* 如果达到了eof，我们需要删除后面的40个字节 */
    if (usemark && eof_reached) {
        if (ftruncate(server.repl_transfer_fd,
            server.repl_transfer_read - CONFIG_RUN_ID_SIZE) == -1)
        {
            serverLog(LL_WARNING,"Error truncating the RDB file received from the master for SYNC: %s", strerror(errno));
            goto error;
        }
    }

    /* 立即把数据sync到磁盘上，如果先保存在内存中最后在落盘，我们会消耗很多内存 */
    if (server.repl_transfer_read >=
        server.repl_transfer_last_fsync_off + REPL_MAX_WRITTEN_BEFORE_FSYNC)
    {
        off_t sync_size = server.repl_transfer_read -
                          server.repl_transfer_last_fsync_off;
        rdb_fsync_range(server.repl_transfer_fd,
            server.repl_transfer_last_fsync_off, sync_size);
        server.repl_transfer_last_fsync_off += sync_size;
    }

    /* 检查传输是否完成了 */
    if (!usemark) {
        if (server.repl_transfer_read == server.repl_transfer_size)
            eof_reached = 1;
    }

    if (eof_reached) {
        int aof_is_enabled = server.aof_state != AOF_OFF;

        if (rename(server.repl_transfer_tmpfile,server.rdb_filename) == -1) {
            serverLog(LL_WARNING,"Failed trying to rename the temp DB into dump.rdb in MASTER <-> REPLICA synchronization: %s", strerror(errno));
            cancelReplicationHandshake();
            return;
        }
        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Flushing old data");
        /* 如果开启了AOF，我们需要暂时停止，否则AOF和RDB一起进行，我们会造成写时复制的灾难行为 */
        if(aof_is_enabled) stopAppendOnly();
        signalFlushedDb(-1);
        // 清空数据库
        emptyDb(
            -1,
            server.repl_slave_lazy_flush ? EMPTYDB_ASYNC : EMPTYDB_NO_FLAGS,
            replicationEmptyDbCallback);
        /* 在把DB异步加载到内存中之前，我们需要删除可读事件处理器，否则当新的数据到达时，
         * 这个事件会被事件循环一直调用 */
        aeDeleteFileEvent(server.el,server.repl_transfer_s,AE_READABLE);
        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Loading DB in memory");
        rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
        /* 重新加载RDB文件 */
        if (rdbLoad(server.rdb_filename,&rsi) != C_OK) {
            serverLog(LL_WARNING,"Failed trying to load the MASTER synchronization DB from disk");
            cancelReplicationHandshake();
            /* 如果我们刚刚关闭了AOF，重新开启 */
            if (aof_is_enabled) restartAOF();
            return;
        }
        /* 最后我们设置主从同步状态为REPL_STATE_CONNECTED，
         * 创建一个新的客户端，在创建时会注册可读事件为readQueryFromClient */
        zfree(server.repl_transfer_tmpfile);
        close(server.repl_transfer_fd);
        replicationCreateMasterClient(server.repl_transfer_s,rsi.repl_stream_db);
        server.repl_state = REPL_STATE_CONNECTED;
        server.repl_down_since = 0;
        /* 在完整重同步后，我们使用master的replid和偏移量，id2和偏移量将会被清空，
         * 因为我们开始了一个新的复制流。 */
        memcpy(server.replid,server.master->replid,sizeof(server.replid));
        server.master_repl_offset = server.master->reploff;
        clearReplicationId2();
        /* 如果需要我们创建复制缓冲区。
         * 不管slaves是否有sub-slaves，都需要有复制缓冲区来支持正确的故障转移  */
        if (server.repl_backlog == NULL) createReplicationBacklog();

        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Finished with success");
        /* 我们已经完成了同步，重启AOF。这将会触发一次AOF重写，当结束时将会开始追加一个新的AOF文件 */
        if (aof_is_enabled) restartAOF();
    }
    return;

error:
    cancelReplicationHandshake();
    return;
}

/* 同步发送一个命令到master或同步读取一个响应。
 * 这个命令返回一个sds字符串表示的结果，当出现错误时第一个字节为- */
#define SYNC_CMD_READ (1<<0)
#define SYNC_CMD_WRITE (1<<1)
#define SYNC_CMD_FULL (SYNC_CMD_READ|SYNC_CMD_WRITE)
char *sendSynchronousCommand(int flags, int fd, ...) {

    /* 创建一个命令发送给master，我们使用一个redis二进制协议来保证要发送的参数争取。
     * 这个函数并非对所有二进制数据都是安全的。 */
    if (flags & SYNC_CMD_WRITE) {
        // 构造redis二进制协议数据
        char *arg;
        va_list ap;
        sds cmd = sdsempty();
        sds cmdargs = sdsempty();
        size_t argslen = 0;
        va_start(ap,fd);

        while(1) {
            arg = va_arg(ap, char*);
            if (arg == NULL) break;

            cmdargs = sdscatprintf(cmdargs,"$%zu\r\n%s\r\n",strlen(arg),arg);
            argslen++;
        }

        va_end(ap);

        cmd = sdscatprintf(cmd,"*%zu\r\n",argslen);
        cmd = sdscatsds(cmd,cmdargs);
        sdsfree(cmdargs);

        /* 同步传输命令到服务器 */
        if (syncWrite(fd,cmd,sdslen(cmd),server.repl_syncio_timeout*1000)
            == -1)
        {
            sdsfree(cmd);
            return sdscatprintf(sdsempty(),"-Writing to master: %s",
                    strerror(errno));
        }
        sdsfree(cmd);
    }

    /* 读取回复 */
    if (flags & SYNC_CMD_READ) {
        char buf[256];

        // 从fd中读取最多256个字节来查找'\n'
        if (syncReadLine(fd,buf,sizeof(buf),server.repl_syncio_timeout*1000)
            == -1)
        {
            return sdscatprintf(sdsempty(),"-Reading from master: %s",
                    strerror(errno));
        }
        server.repl_transfer_lastio = server.unixtime;
        return sdsnew(buf);
    }
    return NULL;
}

/* 如果我们正在重连，尝试一个部分重同步。
 * 如果没有cached master，我们将会发送'PSYNC ? -1'来触发一个全同步，来获取master的
 * runid和复制全局offset。
 *
 * 这个函数被syncWithMaster调用，所以下面的假设成立：
 * 1) 入参fd已经连接上。
 * 2) 这个函数不会关闭fd，然而，当部分重同步成功时，server.master的client结构体将会重用fd。
 *
 * 这个函数有两种行为，通过read_reply控制：
 * 如果read_reply为0，我们将会发送PSYNC命令到master，并且调用方需要在之后使用read_reply=1，
 * 再次调用该函数来读取master的回复。这是为了支持非阻塞的操作，我们在两次事件循环中分别进行读写
 * 来发送命令和获取响应。
 *
 * 当read_reply=0时，即发送命令时，如果发送出错，将会返回PSYNC_WRITE_ERR，否则将会返回
 * PSYNC_WAIT_REPLY，并且调用方需要使用read_reply=1再次调用该函数来读取master的回复。
 * 当read_reply=1时，当还没有有效的数据时，将再次返回PSYNC_WAIT_REPLY，
 *
 * 函数返回值：
 * PSYNC_CONTINUE: 可以进行部分重同步。
 * PSYNC_FULLRESYNC：master支持PSYNC命令，但是需要进行完整重同步。master runid和offset将会被保存。
 * PSYNC_NOT_SUPPORTED： master不理解PSYNC命令，调用方需要使用SYNC命令再次调用。
 * PSYNC_WRITE_ERROR：写命令时出现了错误。
 * PSYNC_WAIT_REPLY：暂时没有有效的数据，需要后续使用read_reply=1再次调用。
 * PSYNC_TRY_LATER： master暂时无法处理，比如正在load数据或者master没联系上自己的master，等等。
 *
 * 副作用：
 * 1) 这个将会移除读事件处理器，除非返回了PSYNC_WAIT_REPLY。
 * 2) server.master_initial_offset将会根据master的reply正确的设置，这个值将会被用来
 *    填充server.master结构体的复制偏移量。
 * */
#define PSYNC_WRITE_ERROR 0
#define PSYNC_WAIT_REPLY 1
#define PSYNC_CONTINUE 2
#define PSYNC_FULLRESYNC 3
#define PSYNC_NOT_SUPPORTED 4
#define PSYNC_TRY_LATER 5
int slaveTryPartialResynchronization(int fd, int read_reply) {
    char *psync_replid;
    char psync_offset[32];
    sds reply;

    if (!read_reply) {      // read_reply=0，向master发送PSYNC
        /* 初始化master_initial_offset=-1来标记不可用。
         * 之后我们如果要做完整重同步我们将会设置其为正确的值，然后这个值将会传递到server.master。*/
        server.master_initial_offset = -1;

        if (server.cached_master) {     // 如果我们有一个缓存的master信息，我们将会使用该信息做部分重同步
            psync_replid = server.cached_master->replid;
            // 注意：PSYNC发送的偏移量是reploff+1，表示slave需要的首个数据
            snprintf(psync_offset,sizeof(psync_offset),"%lld", server.cached_master->reploff+1);
            serverLog(LL_NOTICE,"Trying a partial resynchronization (request %s:%s).", psync_replid, psync_offset);
        } else {        // 如果没有，我们只能进行一个完整重同步
            serverLog(LL_NOTICE,"Partial resynchronization not possible (no cached master)");
            psync_replid = "?";
            memcpy(psync_offset,"-1",3);
        }

        /* 发送PSYNC命令 */
        reply = sendSynchronousCommand(SYNC_CMD_WRITE,fd,"PSYNC",psync_replid,psync_offset,NULL);
        if (reply != NULL) {
            serverLog(LL_WARNING,"Unable to send PSYNC to master: %s",reply);
            sdsfree(reply);
            // 发送PSYNC出错，删除注册的读事件
            aeDeleteFileEvent(server.el,fd,AE_READABLE);
            return PSYNC_WRITE_ERROR;
        }
        return PSYNC_WAIT_REPLY;
    }

    // read_reply=1, 之后后面的逻辑，读取PSYNC的响应
    /* 从响应中读取一行命令回复 */
    reply = sendSynchronousCommand(SYNC_CMD_READ,fd,NULL);
    if (sdslen(reply) == 0) {
        /* 在它接受到PSYNC回复前后，master可能会返回一些空白行用于心跳，我们保持连接即可 */
        sdsfree(reply);
        return PSYNC_WAIT_REPLY;
    }

    aeDeleteFileEvent(server.el,fd,AE_READABLE);

    // +FULLRESYNC bc1621104063d4f46cff756b644c290e80362d3c 238
    if (!strncmp(reply,"+FULLRESYNC",11)) {
        char *replid = NULL, *offset = NULL;

        /* 如果master要求我们做完整重同步，解析reply来提取runid和复制偏移量 */
        replid = strchr(reply,' ');
        if (replid) {
            replid++;
            offset = strchr(replid,' ');
            if (offset) offset++;
        }
        if (!replid || !offset || (offset-replid-1) != CONFIG_RUN_ID_SIZE) {
            serverLog(LL_WARNING,
                "Master replied with wrong +FULLRESYNC syntax.");
            /* 这是一种异常情况，master返回了+FULLRESYNC，说明master支持PSYNC，
             * 但是返回的格式看起来有问题。
             * 为了保证安全，我们把replid置为空, 防止等会重连master时使用一个错误的replid */
            memset(server.master_replid,0,CONFIG_RUN_ID_SIZE+1);
        } else {
            memcpy(server.master_replid, replid, offset-replid-1);
            server.master_replid[CONFIG_RUN_ID_SIZE] = '\0';
            server.master_initial_offset = strtoll(offset,NULL,10);
            serverLog(LL_NOTICE,"Full resync from master: %s:%lld",
                server.master_replid,
                server.master_initial_offset);
        }
        /* 我们要执行完整重同步了，抛弃cached master结构体。 */
        replicationDiscardCachedMaster();
        sdsfree(reply);
        return PSYNC_FULLRESYNC;
    }

    // +CONTINUE <replid> <offset>
    if (!strncmp(reply,"+CONTINUE",9)) {
        /* 部分重同步被接受 */
        serverLog(LL_NOTICE,
            "Successful partial resynchronization with master.");

        /* 检查master宣告的复制偏移量。
         * 如果它改变，我们需要把新的id作为id，并把之前的id作为次id2，并更新second_replid_offset，
         * 这样我们的子slaves可以在断开后使用PSYNC重连我们。 */
        char *start = reply+10;
        char *end = reply+9;
        while(end[0] != '\r' && end[0] != '\n' && end[0] != '\0') end++;
        if (end-start == CONFIG_RUN_ID_SIZE) {
            char new[CONFIG_RUN_ID_SIZE+1];
            memcpy(new,start,CONFIG_RUN_ID_SIZE);
            new[CONFIG_RUN_ID_SIZE] = '\0';

            if (strcmp(new,server.cached_master->replid)) { // masterID改变
                serverLog(LL_WARNING,"Master replication ID changed to %s",new);

                /* 设置oldID作为我们的id2，并更新second_replid_offset */
                memcpy(server.replid2,server.cached_master->replid,
                    sizeof(server.replid2));
                server.second_replid_offset = server.master_repl_offset+1;

                /* 更新ceched->masterID和id */
                memcpy(server.replid,new,sizeof(server.replid));
                memcpy(server.cached_master->replid,new,sizeof(server.replid));

                /* 断开所有的slave，之后它们将会重连，并使用我们新的id */
                disconnectSlaves();
            }
        }

        /* 设置继续复制 */
        sdsfree(reply);
        replicationResurrectCachedMaster(fd);

        /* 如果当前的实例是我们重启的，并且PSYNC的元数据是从持久化文件中读取的，
         * 我们的复制缓冲区应该还没有初始化，创建它。 */
        if (server.repl_backlog == NULL) createReplicationBacklog();
        return PSYNC_CONTINUE;
    }

    /* 如果我们达到了这里说明我们遇到了某些错误。
     * 当错误是临时错误时，我们返回PSYNC_TRY_LATER
     * 当master不支持PSYNC或者我们不清楚的错误时，我们返回PSYNC_NOT_SUPPORTED */

    if (!strncmp(reply,"-NOMASTERLINK",13) ||
        !strncmp(reply,"-LOADING",8))
    {
        serverLog(LL_NOTICE,
            "Master is currently unable to PSYNC "
            "but should be in the future: %s", reply);
        sdsfree(reply);
        return PSYNC_TRY_LATER;
    }

    if (strncmp(reply,"-ERR",4)) {
        /* If it's not an error, log the unexpected event. */
        serverLog(LL_WARNING,
            "Unexpected reply to PSYNC from master: %s", reply);
    } else {
        serverLog(LL_NOTICE,
            "Master does not support PSYNC or is in "
            "error state (reply: %s)", reply);
    }
    sdsfree(reply);
    replicationDiscardCachedMaster();
    return PSYNC_NOT_SUPPORTED;
}

/* 这个函数将会在一个非阻塞的客户端连接到master后触发 */
void syncWithMaster(aeEventLoop *el, int fd, void *privdata, int mask) {
    char tmpfile[256], *err = NULL;
    int dfd = -1, maxtries = 5;
    int sockerr = 0, psync_result;
    socklen_t errlen = sizeof(sockerr);
    UNUSED(el);
    UNUSED(privdata);
    UNUSED(mask);

    /* 当用户将服务器切换到master后，关闭socket */
    if (server.repl_state == REPL_STATE_NONE) {
        close(fd);
        return;
    }

    /* 一个非阻塞的连接可能会触发error，如果出现了错误，我们跳转到error */
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    if (sockerr) {
        serverLog(LL_WARNING,"Error condition on socket for SYNC: %s",
            strerror(sockerr));
        goto error;
    }

    /* 发送一个PING命令检查master是否有能力回复 */
    if (server.repl_state == REPL_STATE_CONNECTING) {
        serverLog(LL_NOTICE,"Non blocking connect for SYNC fired the event.");
        // 删除写事件，我们只关注PONG回复，切换状态
        aeDeleteFileEvent(server.el,fd,AE_WRITABLE);
        server.repl_state = REPL_STATE_RECEIVE_PONG;
        // 发送PING，如果出现了错误，那么就跳转到write_error
        err = sendSynchronousCommand(SYNC_CMD_WRITE,fd,"PING",NULL);
        if (err) goto write_error;
        return;
    }

    /* Receive the PONG command. */
    if (server.repl_state == REPL_STATE_RECEIVE_PONG) {
        err = sendSynchronousCommand(SYNC_CMD_READ,fd,NULL);

        /* +PONG 正常响应
         * --NOAUTH 需要进行验证
         * -ERR operation not permitted 老版本，需要进行验证
         * */
        if (err[0] != '+' &&
            strncmp(err,"-NOAUTH",7) != 0 &&
            strncmp(err,"-ERR operation not permitted",28) != 0)
        {
            serverLog(LL_WARNING,"Error reply to PING from master: '%s'",err);
            sdsfree(err);
            goto error;
        } else {
            serverLog(LL_NOTICE,
                "Master replied to PING, replication can continue...");
        }
        sdsfree(err);
        // 切换到待发送AUTH状态
        server.repl_state = REPL_STATE_SEND_AUTH;
    }

    /* 如果master需要，我们就进行验证，否则我们切换到待发送端口状态 */
    if (server.repl_state == REPL_STATE_SEND_AUTH) {
        if (server.masterauth) {
            err = sendSynchronousCommand(SYNC_CMD_WRITE,fd,"AUTH",server.masterauth,NULL);
            if (err) goto write_error;
            server.repl_state = REPL_STATE_RECEIVE_AUTH;
            return;
        } else {
            server.repl_state = REPL_STATE_SEND_PORT;
        }
    }

    /* 接收AUTH回复，如果通过，切换到待发送端口状态 */
    if (server.repl_state == REPL_STATE_RECEIVE_AUTH) {
        err = sendSynchronousCommand(SYNC_CMD_READ,fd,NULL);
        if (err[0] == '-') {
            serverLog(LL_WARNING,"Unable to AUTH to MASTER: %s",err);
            sdsfree(err);
            goto error;
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_PORT;
    }

    /* 如果配置了server.slave_announce_port，则使用slave_announce_port，否则使用server.port  */
    if (server.repl_state == REPL_STATE_SEND_PORT) {
        sds port = sdsfromlonglong(server.slave_announce_port ?
            server.slave_announce_port : server.port);
        err = sendSynchronousCommand(SYNC_CMD_WRITE,fd,"REPLCONF",
                "listening-port",port, NULL);
        sdsfree(port);
        if (err) goto write_error;
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_PORT;
        return;
    }

    /* 接收 REPLCONF listeniing-port [port] 的响应 */
    if (server.repl_state == REPL_STATE_RECEIVE_PORT) {
        err = sendSynchronousCommand(SYNC_CMD_READ,fd,NULL);
        /*忽略，不是所有redis版本都支持  REPLCONF listening-port. */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF listening-port: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_IP;
    }

    /* 如果没有配置slave-announce-ip，则跳过，master可以直接从socket中知道ip */
    if (server.repl_state == REPL_STATE_SEND_IP &&
        server.slave_announce_ip == NULL)
    {
            server.repl_state = REPL_STATE_SEND_CAPA;
    }

    /* 发送 REPLCONF ip-address [ip] */
    /* 设置slave的ip，这样对于端口转发和NAT的场景，master的INFO命令将能够列出正确的slave的ip */
    if (server.repl_state == REPL_STATE_SEND_IP) {
        err = sendSynchronousCommand(SYNC_CMD_WRITE,fd,"REPLCONF",
                "ip-address",server.slave_announce_ip, NULL);
        if (err) goto write_error;
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_IP;
        return;
    }

    /* 接收 REPLCONF ip-address [ip] 的响应 */
    if (server.repl_state == REPL_STATE_RECEIVE_IP) {
        err = sendSynchronousCommand(SYNC_CMD_READ,fd,NULL);
        /* 忽略，不是所有redis版本都支持 REPLCONF ip-address */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF ip-address: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_CAPA;
    }

    /* 宣告我们的slave支持的能力
     * EOF：支持EOF格式的RDB无盘复制
     * PSYNC2：支持PSYNC v2，所以能够理解 +CONTINUE <new repl ID>
     *
     * master将会忽略它无法理解的能力
     * */
    if (server.repl_state == REPL_STATE_SEND_CAPA) {
        err = sendSynchronousCommand(SYNC_CMD_WRITE,fd,"REPLCONF",
                "capa","eof","capa","psync2",NULL);
        if (err) goto write_error;
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_CAPA;
        return;
    }

    /* 接收CAPA回复 */
    if (server.repl_state == REPL_STATE_RECEIVE_CAPA) {
        err = sendSynchronousCommand(SYNC_CMD_READ,fd,NULL);
        /* 忽略，不是所有redis版本都支持 REPLCONF capa. */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                  "REPLCONF capa: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_PSYNC;
    }

    /* 尝试执行一个部分重同步。
     * 如果没有cached master，我们将会使用PSYNC来开启一个完整重同步，这样我们可以
     * 获得master runid和全局偏移量，我们将会在下次重连时开启一个部分重同步。 */
    if (server.repl_state == REPL_STATE_SEND_PSYNC) {
        if (slaveTryPartialResynchronization(fd,0) == PSYNC_WRITE_ERROR) {
            err = sdsnew("Write error sending the PSYNC command.");
            goto write_error;
        }
        server.repl_state = REPL_STATE_RECEIVE_PSYNC;
        return;
    }

    /* 如果我们到达了这里，我们应该处于REPL_STATE_RECEIVE_PSYNC状态 */
    if (server.repl_state != REPL_STATE_RECEIVE_PSYNC) {
        serverLog(LL_WARNING,"syncWithMaster(): state machine error, "
                             "state should be RECEIVE_PSYNC but is %d",
                             server.repl_state);
        goto error;
    }

    /* 获取PSYNC的响应 */
    psync_result = slaveTryPartialResynchronization(fd,1);
    if (psync_result == PSYNC_WAIT_REPLY) return; /* 暂时没有数据，继续获取 */

    /* master暂时无法进行复制，我们之后需要从零开始复制，所以我们转到err。
     * 当master正在loading或者master未连接到它的master等情况时发生。 */
    if (psync_result == PSYNC_TRY_LATER) goto error;

    /* 注意：如果PSYNC没有返回WAIT_REPLY，它会自己处理卸载可读事件处理器 */

    if (psync_result == PSYNC_CONTINUE) {
        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Master accepted a Partial Resynchronization.");
        return;
    }

    /* PSYNC失败或者不支持：如果我们有sub-slaves，我们希望它们和我们重新进行同步。
     * 因为master可能会传输给我们一个全新的数据集，我们不能增量传播给slaves。 */
    disconnectSlaves(); /* 强制我们的slaves重新同步我们 */
    freeReplicationBacklog(); /* 不允许我们的slaves进行PSYNC */

    /* 不支持PSYNC命令，我们将会在后面用SYNC重试 */
    if (psync_result == PSYNC_NOT_SUPPORTED) {
        serverLog(LL_NOTICE,"Retrying with SYNC...");
        if (syncWrite(fd,"SYNC\r\n",6,server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,"I/O error writing to MASTER: %s",
                strerror(errno));
            goto error;
        }
    }

    /* 准备一个合适的临时文件来接收RDB数据 */
    while(maxtries--) {
        snprintf(tmpfile,256,
            "temp-%d.%ld.rdb",(int)server.unixtime,(long int)getpid());
        dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
        if (dfd != -1) break;
        sleep(1);
    }
    if (dfd == -1) {
        serverLog(LL_WARNING,"Opening the temp file needed for MASTER <-> REPLICA synchronization: %s",strerror(errno));
        goto error;
    }

    /* 设置非阻塞下载RDB文件的可读事件处理器 */
    if (aeCreateFileEvent(server.el,fd, AE_READABLE,readSyncBulkPayload,NULL)
            == AE_ERR)
    {
        serverLog(LL_WARNING,
            "Can't create readable event for SYNC: %s (fd=%d)",
            strerror(errno),fd);
        goto error;
    }

    // 初始化相关状态
    server.repl_state = REPL_STATE_TRANSFER;
    server.repl_transfer_size = -1;
    server.repl_transfer_read = 0;
    server.repl_transfer_last_fsync_off = 0;
    server.repl_transfer_fd = dfd;
    server.repl_transfer_lastio = server.unixtime;
    server.repl_transfer_tmpfile = zstrdup(tmpfile);
    return;

error:      // 如果出现了任何错误，我们将会重置关闭socket，删除ae注册时间，并重置复制状态以重连
    aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
    if (dfd != -1) close(dfd);
    close(fd);
    server.repl_transfer_s = -1;
    server.repl_state = REPL_STATE_CONNECT;
    return;

write_error:     // 处理同步发送命令的错误
    serverLog(LL_WARNING,"Sending command to master in replication handshake: %s", err);
    sdsfree(err);
    goto error;
}

/* 连接上master
 * 1. 创建socket，连接上master
 * 2. 注册该socket的读写时间为syncWithMaster函数
 * 3. 修改复制相关状态
 * */
int connectWithMaster(void) {
    int fd;

    fd = anetTcpNonBlockBestEffortBindConnect(NULL,
        server.masterhost,server.masterport,NET_FIRST_BIND_ADDR);
    if (fd == -1) {
        serverLog(LL_WARNING,"Unable to connect to MASTER: %s",
            strerror(errno));
        return C_ERR;
    }

    if (aeCreateFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE,syncWithMaster,NULL) ==
            AE_ERR)
    {
        close(fd);
        serverLog(LL_WARNING,"Can't create readable event for SYNC");
        return C_ERR;
    }

    server.repl_transfer_lastio = server.unixtime;
    server.repl_transfer_s = fd;
    server.repl_state = REPL_STATE_CONNECTING;
    return C_OK;
}

/* This function can be called when a non blocking connection is currently
 * in progress to undo it.
 * Never call this function directly, use cancelReplicationHandshake() instead.
 */
void undoConnectWithMaster(void) {
    int fd = server.repl_transfer_s;

    aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
    close(fd);
    server.repl_transfer_s = -1;
}

/* Abort the async download of the bulk dataset while SYNC-ing with master.
 * Never call this function directly, use cancelReplicationHandshake() instead.
 */
void replicationAbortSyncTransfer(void) {
    serverAssert(server.repl_state == REPL_STATE_TRANSFER);
    undoConnectWithMaster();
    close(server.repl_transfer_fd);
    unlink(server.repl_transfer_tmpfile);
    zfree(server.repl_transfer_tmpfile);
}

/* This function aborts a non blocking replication attempt if there is one
 * in progress, by canceling the non-blocking connect attempt or
 * the initial bulk transfer.
 *
 * If there was a replication handshake in progress 1 is returned and
 * the replication state (server.repl_state) set to REPL_STATE_CONNECT.
 *
 * Otherwise zero is returned and no operation is perforemd at all. */
/* 这个函数将会通过取消一个非阻塞的连接或者初始化的传输过程，来终止一个正在进行中的复制。
 *
 * 如果当前有一个正在进行中的复制，将会取下并设置为未连接状态，否则返回0且什么都不做。
 * */
int cancelReplicationHandshake(void) {
    if (server.repl_state == REPL_STATE_TRANSFER) {
        replicationAbortSyncTransfer();
        server.repl_state = REPL_STATE_CONNECT;
    } else if (server.repl_state == REPL_STATE_CONNECTING ||
               slaveIsInHandshakeState())
    {
        undoConnectWithMaster();
        server.repl_state = REPL_STATE_CONNECT;
    } else {
        return 0;
    }
    return 1;
}

/* 设置指定的ip:port为master */
void replicationSetMaster(char *ip, int port) {
    int was_master = server.masterhost == NULL;

    sdsfree(server.masterhost);
    server.masterhost = sdsnew(ip);
    server.masterport = port;
    if (server.master) {
        freeClient(server.master);
    }
    disconnectAllBlockedClients(); /* Clients blocked in master, now slave. */

    /* Force our slaves to resync with us as well. They may hopefully be able
     * to partially resync with us, but we can notify the replid change. */
    disconnectSlaves();
    cancelReplicationHandshake();
    /* Before destroying our master state, create a cached master using
     * our own parameters, to later PSYNC with the new master. */
    /* 在销毁master状态之前，使用我们自己的参数创建一个cached状态，用于等会部分重同步 */
    if (was_master) replicationCacheMasterUsingMyself();
    // 设置当前复制状态为待连接
    server.repl_state = REPL_STATE_CONNECT;
}

/* 取消复制，将当前服务器设置为master */
void replicationUnsetMaster(void) {
    if (server.masterhost == NULL) return; /* Nothing to do. */
    sdsfree(server.masterhost);
    server.masterhost = NULL;
    /* When a slave is turned into a master, the current replication ID
     * (that was inherited from the master at synchronization time) is
     * used as secondary ID up to the current offset, and a new replication
     * ID is created to continue with a new replication history. */
    shiftReplicationId();
    if (server.master) freeClient(server.master);
    replicationDiscardCachedMaster();
    cancelReplicationHandshake();
    /* Disconnecting all the slaves is required: we need to inform slaves
     * of the replication ID change (see shiftReplicationId() call). However
     * the slaves will be able to partially resync with us, so it will be
     * a very fast reconnection. */
    disconnectSlaves();
    server.repl_state = REPL_STATE_NONE;

    /* We need to make sure the new master will start the replication stream
     * with a SELECT statement. This is forced after a full resync, but
     * with PSYNC version 2, there is no need for full resync after a
     * master switch. */
    server.slaveseldb = -1;

    /* Once we turn from slave to master, we consider the starting time without
     * slaves (that is used to count the replication backlog time to live) as
     * starting from now. Otherwise the backlog will be freed after a
     * failover if slaves do not connect immediately. */
    server.repl_no_slaves_since = server.unixtime;
}

/* This function is called when the slave lose the connection with the
 * master into an unexpected way. */
/* 当这个slave以一种非预期的方式-网络断开等失去了和master的连接时，我们将会调用这个函数 */
void replicationHandleMasterDisconnection(void) {
    server.master = NULL;
    server.repl_state = REPL_STATE_CONNECT;
    server.repl_down_since = server.unixtime;
    /* We lost connection with our master, don't disconnect slaves yet,
     * maybe we'll be able to PSYNC with our master later. We'll disconnect
     * the slaves only if we'll have to do a full resync with our master. */
}

void replicaofCommand(client *c) {
    /*  集群模式下不允许执行SLAVEOF，因为复制是使用当前master节点地址自动配置的 */
    if (server.cluster_enabled) {
        addReplyError(c,"REPLICAOF not allowed in cluster mode.");
        return;
    }

    /* slave of no one 将会使当前服务器变更为master，否则将会设置master的地址 */
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            replicationUnsetMaster();
            sds client = catClientInfoString(sdsempty(),c);
            serverLog(LL_NOTICE,"MASTER MODE enabled (user request from '%s')",
                client);
            sdsfree(client);
        }
    } else {
        long port;

        if ((getLongFromObjectOrReply(c, c->argv[2], &port, NULL) != C_OK))
            return;

        /* 如果当前服务器已经将要跟随的机器设置为主服务器，则直接返回+OK */
        if (server.masterhost && !strcasecmp(server.masterhost,c->argv[1]->ptr)
            && server.masterport == port) {
            serverLog(LL_NOTICE,"REPLICAOF would result into synchronization with the master we are already connected with. No operation performed.");
            addReplySds(c,sdsnew("+OK Already connected to specified master\r\n"));
            return;
        }
        /* 如果跟随一个新的master，则继续 */
        replicationSetMaster(c->argv[1]->ptr, port);
        sds client = catClientInfoString(sdsempty(),c);
        serverLog(LL_NOTICE,"REPLICAOF %s:%d enabled (user request from '%s')",
            server.masterhost, server.masterport, client);
        sdsfree(client);
    }
    addReply(c,shared.ok);
}

/* ROLE command: provide information about the role of the instance
 * (master or slave) and additional information related to replication
 * in an easy to process format. */
void roleCommand(client *c) {
    if (server.masterhost == NULL) {
        listIter li;
        listNode *ln;
        void *mbcount;
        int slaves = 0;

        addReplyMultiBulkLen(c,3);
        addReplyBulkCBuffer(c,"master",6);
        addReplyLongLong(c,server.master_repl_offset);
        mbcount = addDeferredMultiBulkLength(c);
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            char ip[NET_IP_STR_LEN], *slaveip = slave->slave_ip;

            if (slaveip[0] == '\0') {
                if (anetPeerToString(slave->fd,ip,sizeof(ip),NULL) == -1)
                    continue;
                slaveip = ip;
            }
            if (slave->replstate != SLAVE_STATE_ONLINE) continue;
            addReplyMultiBulkLen(c,3);
            addReplyBulkCString(c,slaveip);
            addReplyBulkLongLong(c,slave->slave_listening_port);
            addReplyBulkLongLong(c,slave->repl_ack_off);
            slaves++;
        }
        setDeferredMultiBulkLength(c,mbcount,slaves);
    } else {
        char *slavestate = NULL;

        addReplyMultiBulkLen(c,5);
        addReplyBulkCBuffer(c,"slave",5);
        addReplyBulkCString(c,server.masterhost);
        addReplyLongLong(c,server.masterport);
        if (slaveIsInHandshakeState()) {
            slavestate = "handshake";
        } else {
            switch(server.repl_state) {
            case REPL_STATE_NONE: slavestate = "none"; break;
            case REPL_STATE_CONNECT: slavestate = "connect"; break;
            case REPL_STATE_CONNECTING: slavestate = "connecting"; break;
            case REPL_STATE_TRANSFER: slavestate = "sync"; break;
            case REPL_STATE_CONNECTED: slavestate = "connected"; break;
            default: slavestate = "unknown"; break;
            }
        }
        addReplyBulkCString(c,slavestate);
        addReplyLongLong(c,server.master ? server.master->reploff : -1);
    }
}

/* 把REPLCONF ACK命令写入到输出缓冲区中。如果我们没有连接到master，命令无影响 */
void replicationSendAck(void) {
    client *c = server.master;

    if (c != NULL) {
        // 必须先设置CLIENT_MASTER_FORCE_REPLY，否则addReply会写入失败
        c->flags |= CLIENT_MASTER_FORCE_REPLY;
        addReplyMultiBulkLen(c,3);
        addReplyBulkCString(c,"REPLCONF");
        addReplyBulkCString(c,"ACK");
        addReplyBulkLongLong(c,c->reploff);
        // 写入成功，设置会不响应master命令的状态
        c->flags &= ~CLIENT_MASTER_FORCE_REPLY;
    }
}

/* ---------------------- MASTER CACHING FOR PSYNC -------------------------- */

/* In order to implement partial synchronization we need to be able to cache
 * our master's client structure after a transient disconnection.
 * It is cached into server.cached_master and flushed away using the following
 * functions. */

/* This function is called by freeClient() in order to cache the master
 * client structure instead of destroying it. freeClient() will return
 * ASAP after this function returns, so every action needed to avoid problems
 * with a client that is really "suspended" has to be done by this function.
 *
 * The other functions that will deal with the cached master are:
 *
 * replicationDiscardCachedMaster() that will make sure to kill the client
 * as for some reason we don't want to use it in the future.
 *
 * replicationResurrectCachedMaster() that is used after a successful PSYNC
 * handshake in order to reactivate the cached master.
 */
void replicationCacheMaster(client *c) {
    serverAssert(server.master != NULL && server.cached_master == NULL);
    serverLog(LL_NOTICE,"Caching the disconnected master state.");

    /* Unlink the client from the server structures. */
    unlinkClient(c);

    /* Reset the master client so that's ready to accept new commands:
     * we want to discard te non processed query buffers and non processed
     * offsets, including pending transactions, already populated arguments,
     * pending outputs to the master. */
    sdsclear(server.master->querybuf);
    sdsclear(server.master->pending_querybuf);
    server.master->read_reploff = server.master->reploff;
    if (c->flags & CLIENT_MULTI) discardTransaction(c);
    listEmpty(c->reply);
    c->sentlen = 0;
    c->reply_bytes = 0;
    c->bufpos = 0;
    resetClient(c);

    /* Save the master. Server.master will be set to null later by
     * replicationHandleMasterDisconnection(). */
    server.cached_master = server.master;

    /* Invalidate the Peer ID cache. */
    if (c->peerid) {
        sdsfree(c->peerid);
        c->peerid = NULL;
    }

    /* Caching the master happens instead of the actual freeClient() call,
     * so make sure to adjust the replication state. This function will
     * also set server.master to NULL. */
    replicationHandleMasterDisconnection();
}

/* This function is called when a master is turend into a slave, in order to
 * create from scratch a cached master for the new client, that will allow
 * to PSYNC with the slave that was promoted as the new master after a
 * failover.
 *
 * Assuming this instance was previously the master instance of the new master,
 * the new master will accept its replication ID, and potentiall also the
 * current offset if no data was lost during the failover. So we use our
 * current replication ID and offset in order to synthesize a cached master. */
void replicationCacheMasterUsingMyself(void) {
    /* The master client we create can be set to any DBID, because
     * the new master will start its replication stream with SELECT. */
    server.master_initial_offset = server.master_repl_offset;
    replicationCreateMasterClient(-1,-1);

    /* Use our own ID / offset. */
    memcpy(server.master->replid, server.replid, sizeof(server.replid));

    /* Set as cached master. */
    unlinkClient(server.master);
    server.cached_master = server.master;
    server.master = NULL;
    serverLog(LL_NOTICE,"Before turning into a replica, using my master parameters to synthesize a cached master: I may be able to synchronize with the new master with just a partial transfer.");
}

/* Free a cached master, called when there are no longer the conditions for
 * a partial resync on reconnection. */
void replicationDiscardCachedMaster(void) {
    if (server.cached_master == NULL) return;

    serverLog(LL_NOTICE,"Discarding previously cached master state.");
    server.cached_master->flags &= ~CLIENT_MASTER;
    freeClient(server.cached_master);
    server.cached_master = NULL;
}

/* 复活cached master为当前的master，并使用一个新的文件描述符作为socket参数。
 * 这个函数在成功设置部分重同步时调用，我们可以继续接受master剩下的数据。
 * */
void replicationResurrectCachedMaster(int newfd) {
    server.master = server.cached_master;
    server.cached_master = NULL;
    server.master->fd = newfd;
    server.master->flags &= ~(CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP);
    server.master->authenticated = 1;
    server.master->lastinteraction = server.unixtime;
    server.repl_state = REPL_STATE_CONNECTED;
    server.repl_down_since = 0;

    /* 重新增加client到链表中 */
    linkClient(server.master);
    if (aeCreateFileEvent(server.el, newfd, AE_READABLE,
                          readQueryFromClient, server.master)) {
        serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the readable handler: %s", strerror(errno));
        freeClientAsync(server.master); /* Close ASAP. */
    }

    /* 如果有未发送的数据在写缓冲区中，我们需要安装可写事件处理器 */
    if (clientHasPendingReplies(server.master)) {
        if (aeCreateFileEvent(server.el, newfd, AE_WRITABLE,
                          sendReplyToClient, server.master)) {
            serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the writable handler: %s", strerror(errno));
            freeClientAsync(server.master); /* Close ASAP. */
        }
    }
}

/* ------------------------- MIN-SLAVES-TO-WRITE  --------------------------- */

/* This function counts the number of slaves with lag <= min-slaves-max-lag.
 * If the option is active, the server will prevent writes if there are not
 * enough connected slaves with the specified lag (or less). */
void refreshGoodSlavesCount(void) {
    listIter li;
    listNode *ln;
    int good = 0;

    if (!server.repl_min_slaves_to_write ||
        !server.repl_min_slaves_max_lag) return;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        time_t lag = server.unixtime - slave->repl_ack_time;

        if (slave->replstate == SLAVE_STATE_ONLINE &&
            lag <= server.repl_min_slaves_max_lag) good++;
    }
    server.repl_good_slaves_count = good;
}

/* ----------------------- REPLICATION SCRIPT CACHE --------------------------
 * The goal of this code is to keep track of scripts already sent to every
 * connected slave, in order to be able to replicate EVALSHA as it is without
 * translating it to EVAL every time it is possible.
 *
 * We use a capped collection implemented by a hash table for fast lookup
 * of scripts we can send as EVALSHA, plus a linked list that is used for
 * eviction of the oldest entry when the max number of items is reached.
 *
 * We don't care about taking a different cache for every different slave
 * since to fill the cache again is not very costly, the goal of this code
 * is to avoid that the same big script is trasmitted a big number of times
 * per second wasting bandwidth and processor speed, but it is not a problem
 * if we need to rebuild the cache from scratch from time to time, every used
 * script will need to be transmitted a single time to reappear in the cache.
 *
 * This is how the system works:
 *
 * 1) Every time a new slave connects, we flush the whole script cache.
 * 2) We only send as EVALSHA what was sent to the master as EVALSHA, without
 *    trying to convert EVAL into EVALSHA specifically for slaves.
 * 3) Every time we trasmit a script as EVAL to the slaves, we also add the
 *    corresponding SHA1 of the script into the cache as we are sure every
 *    slave knows about the script starting from now.
 * 4) On SCRIPT FLUSH command, we replicate the command to all the slaves
 *    and at the same time flush the script cache.
 * 5) When the last slave disconnects, flush the cache.
 * 6) We handle SCRIPT LOAD as well since that's how scripts are loaded
 *    in the master sometimes.
 */

/* Initialize the script cache, only called at startup. */
void replicationScriptCacheInit(void) {
    server.repl_scriptcache_size = 10000;
    server.repl_scriptcache_dict = dictCreate(&replScriptCacheDictType,NULL);
    server.repl_scriptcache_fifo = listCreate();
}

/* Empty the script cache. Should be called every time we are no longer sure
 * that every slave knows about all the scripts in our set, or when the
 * current AOF "context" is no longer aware of the script. In general we
 * should flush the cache:
 *
 * 1) Every time a new slave reconnects to this master and performs a
 *    full SYNC (PSYNC does not require flushing).
 * 2) Every time an AOF rewrite is performed.
 * 3) Every time we are left without slaves at all, and AOF is off, in order
 *    to reclaim otherwise unused memory.
 */
void replicationScriptCacheFlush(void) {
    dictEmpty(server.repl_scriptcache_dict,NULL);
    listRelease(server.repl_scriptcache_fifo);
    server.repl_scriptcache_fifo = listCreate();
}

/* Add an entry into the script cache, if we reach max number of entries the
 * oldest is removed from the list. */
void replicationScriptCacheAdd(sds sha1) {
    int retval;
    sds key = sdsdup(sha1);

    /* Evict oldest. */
    if (listLength(server.repl_scriptcache_fifo) == server.repl_scriptcache_size)
    {
        listNode *ln = listLast(server.repl_scriptcache_fifo);
        sds oldest = listNodeValue(ln);

        retval = dictDelete(server.repl_scriptcache_dict,oldest);
        serverAssert(retval == DICT_OK);
        listDelNode(server.repl_scriptcache_fifo,ln);
    }

    /* Add current. */
    retval = dictAdd(server.repl_scriptcache_dict,key,NULL);
    listAddNodeHead(server.repl_scriptcache_fifo,key);
    serverAssert(retval == DICT_OK);
}

/* Returns non-zero if the specified entry exists inside the cache, that is,
 * if all the slaves are aware of this script SHA1. */
int replicationScriptCacheExists(sds sha1) {
    return dictFind(server.repl_scriptcache_dict,sha1) != NULL;
}

/* ----------------------- SYNCHRONOUS REPLICATION --------------------------
 * Redis synchronous replication design can be summarized in points:
 *
 * - Redis masters have a global replication offset, used by PSYNC.
 * - Master increment the offset every time new commands are sent to slaves.
 * - Slaves ping back masters with the offset processed so far.
 *
 * So synchronous replication adds a new WAIT command in the form:
 *
 *   WAIT <num_replicas> <milliseconds_timeout>
 *
 * That returns the number of replicas that processed the query when
 * we finally have at least num_replicas, or when the timeout was
 * reached.
 *
 * The command is implemented in this way:
 *
 * - Every time a client processes a command, we remember the replication
 *   offset after sending that command to the slaves.
 * - When WAIT is called, we ask slaves to send an acknowledgement ASAP.
 *   The client is blocked at the same time (see blocked.c).
 * - Once we receive enough ACKs for a given offset or when the timeout
 *   is reached, the WAIT command is unblocked and the reply sent to the
 *   client.
 */

/* This just set a flag so that we broadcast a REPLCONF GETACK command
 * to all the slaves in the beforeSleep() function. Note that this way
 * we "group" all the clients that want to wait for synchronouns replication
 * in a given event loop iteration, and send a single GETACK for them all. */
void replicationRequestAckFromSlaves(void) {
    server.get_ack_from_slaves = 1;
}

/* Return the number of slaves that already acknowledged the specified
 * replication offset. */
int replicationCountAcksByOffset(long long offset) {
    listIter li;
    listNode *ln;
    int count = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate != SLAVE_STATE_ONLINE) continue;
        if (slave->repl_ack_off >= offset) count++;
    }
    return count;
}

/* WAIT for N replicas to acknowledge the processing of our latest
 * write command (and all the previous commands). */
void waitCommand(client *c) {
    mstime_t timeout;
    long numreplicas, ackreplicas;
    long long offset = c->woff;

    if (server.masterhost) {
        addReplyError(c,"WAIT cannot be used with replica instances. Please also note that since Redis 4.0 if a replica is configured to be writable (which is not the default) writes to replicas are just local and are not propagated.");
        return;
    }

    /* Argument parsing. */
    if (getLongFromObjectOrReply(c,c->argv[1],&numreplicas,NULL) != C_OK)
        return;
    if (getTimeoutFromObjectOrReply(c,c->argv[2],&timeout,UNIT_MILLISECONDS)
        != C_OK) return;

    /* First try without blocking at all. */
    ackreplicas = replicationCountAcksByOffset(c->woff);
    if (ackreplicas >= numreplicas || c->flags & CLIENT_MULTI) {
        addReplyLongLong(c,ackreplicas);
        return;
    }

    /* Otherwise block the client and put it into our list of clients
     * waiting for ack from slaves. */
    c->bpop.timeout = timeout;
    c->bpop.reploffset = offset;
    c->bpop.numreplicas = numreplicas;
    listAddNodeTail(server.clients_waiting_acks,c);
    blockClient(c,BLOCKED_WAIT);

    /* Make sure that the server will send an ACK request to all the slaves
     * before returning to the event loop. */
    replicationRequestAckFromSlaves();
}

/* This is called by unblockClient() to perform the blocking op type
 * specific cleanup. We just remove the client from the list of clients
 * waiting for replica acks. Never call it directly, call unblockClient()
 * instead. */
void unblockClientWaitingReplicas(client *c) {
    listNode *ln = listSearchKey(server.clients_waiting_acks,c);
    serverAssert(ln != NULL);
    listDelNode(server.clients_waiting_acks,ln);
}

/* Check if there are clients blocked in WAIT that can be unblocked since
 * we received enough ACKs from slaves. */
void processClientsWaitingReplicas(void) {
    long long last_offset = 0;
    int last_numreplicas = 0;

    listIter li;
    listNode *ln;

    listRewind(server.clients_waiting_acks,&li);
    while((ln = listNext(&li))) {
        client *c = ln->value;

        /* Every time we find a client that is satisfied for a given
         * offset and number of replicas, we remember it so the next client
         * may be unblocked without calling replicationCountAcksByOffset()
         * if the requested offset / replicas were equal or less. */
        if (last_offset && last_offset > c->bpop.reploffset &&
                           last_numreplicas > c->bpop.numreplicas)
        {
            unblockClient(c);
            addReplyLongLong(c,last_numreplicas);
        } else {
            int numreplicas = replicationCountAcksByOffset(c->bpop.reploffset);

            if (numreplicas >= c->bpop.numreplicas) {
                last_offset = c->bpop.reploffset;
                last_numreplicas = numreplicas;
                unblockClient(c);
                addReplyLongLong(c,numreplicas);
            }
        }
    }
}

/* Return the slave replication offset for this instance, that is
 * the offset for which we already processed the master replication stream. */
long long replicationGetSlaveOffset(void) {
    long long offset = 0;

    if (server.masterhost != NULL) {
        if (server.master) {
            offset = server.master->reploff;
        } else if (server.cached_master) {
            offset = server.cached_master->reploff;
        }
    }
    /* offset may be -1 when the master does not support it at all, however
     * this function is designed to return an offset that can express the
     * amount of data processed by the master, so we return a positive
     * integer. */
    if (offset < 0) offset = 0;
    return offset;
}

/* --------------------------- REPLICATION CRON  ---------------------------- */

/* 复制核心函数，每秒执行一次，在serverCron中调用 */
void replicationCron(void) {
    static long long replication_cron_loops = 0;

    /* 当前实例作为slave，正准备握手或者正在握手：[2,13]，超时，则取消当前握手，重连 */
    if (server.masterhost &&
        (server.repl_state == REPL_STATE_CONNECTING ||
         slaveIsInHandshakeState()) &&
         (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"Timeout connecting to the MASTER...");
        cancelReplicationHandshake();
    }

    /* 当前实例作为slave，正在接受RDB: [14]，如果超时，取消当前握手，重连 */
    if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"Timeout receiving bulk data from MASTER... If the problem persists try to set the 'repl-timeout' parameter in redis.conf to a larger value.");
        cancelReplicationHandshake();
    }

    /* 当前实例作为slave, 正在进行正常的复制，但是出现了超时，则释放master客户端，重连 */
    if (server.masterhost && server.repl_state == REPL_STATE_CONNECTED &&
        (time(NULL)-server.master->lastinteraction) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"MASTER timeout: no data nor PING received...");
        freeClient(server.master);
    }

    /* 当前实例作为slave, 需要连接一个master */
    if (server.repl_state == REPL_STATE_CONNECT) {
        serverLog(LL_NOTICE,"Connecting to MASTER %s:%d",
            server.masterhost, server.masterport);
        if (connectWithMaster() == C_OK) {
            serverLog(LL_NOTICE,"MASTER <-> REPLICA sync started");
        }
    }

    /* 当前实例作为slave, 并且master可以理解PSYNC，发送REPLCONf ACK给master，每秒1次，作用：
     * 1) slave->master的心跳
     * 2) 同步给master当前slave的复制偏移量
     * 注意：如果master不支持PSYNC和复制偏移量。
     * */
    if (server.masterhost && server.master &&
        !(server.master->flags & CLIENT_PRE_PSYNC))
        replicationSendAck();

    /* 如果我们有slaves，不断地PING它们。
     * 这样slaves可以显式的实现一个到master的超时，并且能在TCP不可用时，监测到断连。 */
    listIter li;
    listNode *ln;
    robj *ping_argv[1];

    /* 当时实例作为master向slaves发送PING，每repl_ping_slave_period=10s一次
     * 注意：这个功能实际上是通过写入到复制积压缓冲区中实现的，而不是直接发送给slave。
     * 这意味着，在复制同步期间，所有的PING将会在slave同步期间不会真正发送给slave，
     * 而是在slave的复制状态变更为ONLINE时才发送。
     * */
    if ((replication_cron_loops % server.repl_ping_slave_period) == 0 &&
        listLength(server.slaves))
    {
        ping_argv[0] = createStringObject("PING",4);
        replicationFeedSlaves(server.slaves, server.slaveseldb,
            ping_argv, 1);
        decrRefCount(ping_argv[0]);
    }

    /* 对于presync阶段的slaves，它们正在等待RDB文件，发送一个空行。
     * 对于presync阶段的slaves，它们正在等待RDB文件，而RDB生成的时间可能很长，我们需要让slave
     * 能够知道这段时间内到master的连接正常，这是必要的。
     * 注意：在线状态时master->slave的心跳是通过复制流中的PING实现的，而sub-slaves间的复制流是
     * 是从顶级master继承的，此时还在presync阶段，没有复制流传输，所以PING心跳无法用于presync阶段。
     * 这个presync阶段的newline的心跳机制不会影响复制流偏移量。
     * 这个newline将会被slave忽略，但是会刷新slave和master的最后交互时间来防止超时，我们每秒发送一次。
     * */
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        int is_presync =
            (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START ||
            (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END &&
             server.rdb_child_type != RDB_CHILD_TYPE_SOCKET));

        if (is_presync) {
            if (write(slave->fd, "\n", 1) == -1) {
                /* 不必关心socket errors，这只是用于RDB生成期间的心跳 */
            }
        }
    }

    /* 断开所有超时的slaves */
    if (listLength(server.slaves)) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            if (slave->replstate != SLAVE_STATE_ONLINE) continue;
            if (slave->flags & CLIENT_PRE_PSYNC) continue;
            if ((server.unixtime - slave->repl_ack_time) > server.repl_timeout)
            {
                serverLog(LL_WARNING, "Disconnecting timedout replica: %s",
                    replicationGetSlaveName(slave));
                freeClient(slave);
            }
        }
    }

    /* If this is a master without attached slaves and there is a replication
     * backlog active, in order to reclaim memory we can free it after some
     * (configured) time. Note that this cannot be done for slaves: slaves
     * without sub-slaves attached should still accumulate data into the
     * backlog, in order to reply to PSYNC queries if they are turned into
     * masters after a failover. */
    if (listLength(server.slaves) == 0 && server.repl_backlog_time_limit &&
        server.repl_backlog && server.masterhost == NULL)
    {
        time_t idle = server.unixtime - server.repl_no_slaves_since;

        if (idle > server.repl_backlog_time_limit) {
            /* When we free the backlog, we always use a new
             * replication ID and clear the ID2. This is needed
             * because when there is no backlog, the master_repl_offset
             * is not updated, but we would still retain our replication
             * ID, leading to the following problem:
             *
             * 1. We are a master instance.
             * 2. Our slave is promoted to master. It's repl-id-2 will
             *    be the same as our repl-id.
             * 3. We, yet as master, receive some updates, that will not
             *    increment the master_repl_offset.
             * 4. Later we are turned into a slave, connect to the new
             *    master that will accept our PSYNC request by second
             *    replication ID, but there will be data inconsistency
             *    because we received writes. */
            changeReplicationId();
            clearReplicationId2();
            freeReplicationBacklog();
            serverLog(LL_NOTICE,
                "Replication backlog freed after %d seconds "
                "without connected replicas.",
                (int) server.repl_backlog_time_limit);
        }
    }

    /* If AOF is disabled and we no longer have attached slaves, we can
     * free our Replication Script Cache as there is no need to propagate
     * EVALSHA at all. */
    if (listLength(server.slaves) == 0 &&
        server.aof_state == AOF_OFF &&
        listLength(server.repl_scriptcache_fifo) != 0)
    {
        replicationScriptCacheFlush();
    }

    /* Start a BGSAVE good for replication if we have slaves in
     * WAIT_BGSAVE_START state.
     *
     * In case of diskless replication, we make sure to wait the specified
     * number of seconds (according to configuration) so that other slaves
     * have the time to arrive before we start streaming. */
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1) {
        time_t idle, max_idle = 0;
        int slaves_waiting = 0;
        int mincapa = -1;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                idle = server.unixtime - slave->lastinteraction;
                if (idle > max_idle) max_idle = idle;
                slaves_waiting++;
                mincapa = (mincapa == -1) ? slave->slave_capa :
                                            (mincapa & slave->slave_capa);
            }
        }

        if (slaves_waiting &&
            (!server.repl_diskless_sync ||
             max_idle > server.repl_diskless_sync_delay))
        {
            /* Start the BGSAVE. The called function may start a
             * BGSAVE with socket target or disk target depending on the
             * configuration and slaves capabilities. */
            startBgsaveForReplication(mincapa);
        }
    }

    /* Refresh the number of slaves with lag <= min-slaves-max-lag. */
    refreshGoodSlavesCount();
    replication_cron_loops++; /* Incremented with frequency 1 HZ. */
}
