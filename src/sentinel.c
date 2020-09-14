/* Redis Sentinel implementation
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
#include "hiredis.h"
#include "async.h"

#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>

extern char **environ;

#define REDIS_SENTINEL_PORT 26379

/* ======================== Sentinel global state =========================== */

/* Address object, used to describe an ip:port pair. */
typedef struct sentinelAddr {
    char *ip;
    int port;
} sentinelAddr;

/* 正在监控的RedisInstance对象 */
#define SRI_MASTER  (1<<0)  /* master */
#define SRI_SLAVE   (1<<1)  /* slave */
#define SRI_SENTINEL (1<<2) /* sentinel */
#define SRI_S_DOWN (1<<3)   /* 主观下线 */
#define SRI_O_DOWN (1<<4)   /* 客观下线 */
#define SRI_MASTER_DOWN (1<<5) /* 当前sentinel认为master下线 */
#define SRI_FAILOVER_IN_PROGRESS (1<<6) /* 对于这个master，正在进行故障转移 */
#define SRI_PROMOTED (1<<7)            /* 这个slave已经被选为待晋升 */
#define SRI_RECONF_SENT (1<<8)     /* SLAVEOF <newmaster>已经发送. */
#define SRI_RECONF_INPROG (1<<9)   /* slave正在同步master */
#define SRI_RECONF_DONE (1<<10)     /* slave已经完成到master的同步 */
#define SRI_FORCE_FAILOVER (1<<11)  /* Force failover with master up. */
#define SRI_SCRIPT_KILL_SENT (1<<12) /* SCRIPT KILL already sent on -BUSY */

/* Note: times are in milliseconds. */
#define SENTINEL_INFO_PERIOD 10000
#define SENTINEL_PING_PERIOD 1000
#define SENTINEL_ASK_PERIOD 1000
#define SENTINEL_PUBLISH_PERIOD 2000
#define SENTINEL_DEFAULT_DOWN_AFTER 30000
#define SENTINEL_HELLO_CHANNEL "__sentinel__:hello"
#define SENTINEL_TILT_TRIGGER 2000
#define SENTINEL_TILT_PERIOD (SENTINEL_PING_PERIOD*30)
#define SENTINEL_DEFAULT_SLAVE_PRIORITY 100
#define SENTINEL_SLAVE_RECONF_TIMEOUT 10000
#define SENTINEL_DEFAULT_PARALLEL_SYNCS 1
#define SENTINEL_MIN_LINK_RECONNECT_PERIOD 15000
#define SENTINEL_DEFAULT_FAILOVER_TIMEOUT (60*3*1000)
#define SENTINEL_MAX_PENDING_COMMANDS 100
#define SENTINEL_ELECTION_TIMEOUT 10000
#define SENTINEL_MAX_DESYNC 1000
#define SENTINEL_DEFAULT_DENY_SCRIPTS_RECONFIG 1

/* Failover machine different states. */
#define SENTINEL_FAILOVER_STATE_NONE 0  /* 没有故障转移在进行中 */
#define SENTINEL_FAILOVER_STATE_WAIT_START 1  /* 等待开始一个故障转移 Wait for failover_start_time*/
#define SENTINEL_FAILOVER_STATE_SELECT_SLAVE 2 /* 正选择一个slave晋升 */
#define SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE 3 /* 发送slaveof no one给slave */
#define SENTINEL_FAILOVER_STATE_WAIT_PROMOTION 4 /* 等待所选择的slave晋升完成 */
#define SENTINEL_FAILOVER_STATE_RECONF_SLAVES 5 /* 正在重新配置其它salves：SLAVEOF newmaster */
#define SENTINEL_FAILOVER_STATE_UPDATE_CONFIG 6 /* 其它slave重新配置完成，监视晋升的slave . */

#define SENTINEL_MASTER_LINK_STATUS_UP 0
#define SENTINEL_MASTER_LINK_STATUS_DOWN 1

/* Generic flags that can be used with different functions.
 * They use higher bits to avoid colliding with the function specific
 * flags. */
#define SENTINEL_NO_FLAGS 0
#define SENTINEL_GENERATE_EVENT (1<<16)
#define SENTINEL_LEADER (1<<17)
#define SENTINEL_OBSERVER (1<<18)

/* Script execution flags and limits. */
#define SENTINEL_SCRIPT_NONE 0
#define SENTINEL_SCRIPT_RUNNING 1
#define SENTINEL_SCRIPT_MAX_QUEUE 256
#define SENTINEL_SCRIPT_MAX_RUNNING 16
#define SENTINEL_SCRIPT_MAX_RUNTIME 60000 /* 60 seconds max exec time. */
#define SENTINEL_SCRIPT_MAX_RETRY 10
#define SENTINEL_SCRIPT_RETRY_DELAY 30000 /* 30 seconds between retries. */

/* SENTINEL SIMULATE-FAILURE command flags. */
#define SENTINEL_SIMFAILURE_NONE 0
#define SENTINEL_SIMFAILURE_CRASH_AFTER_ELECTION (1<<0)
#define SENTINEL_SIMFAILURE_CRASH_AFTER_PROMOTION (1<<1)

/* 一条到sentinelRedisInstance的连接。我们可能会有一个sentinel集合监控很多master。
 * 比如5个sentinel监控100个master，那么对于100个master如果我们都各创建5个到sentinel的instanceLink，
 * 我们将会创建500个instanceLink，但实际上我们只需要创建5个到sentinel的instanceLink共享就行了，
 * refcount被用来实现共享，这样不仅用5个instanceLink代替了500个instanceLink，还用5个PING代替了500个PING。
 *
 * instanceLink的共享仅用于sentinels，master和slave的refcount总是1。 */
typedef struct instanceLink {
    int refcount;          /* 这个连接被多少sentinelRedisInstance共用 */
    int disconnected;      /* 如果cc或pc连接需要重连 */
    int pending_commands;  /* 这条连接上正在等待reply的数量 */
    redisAsyncContext *cc; /* 命令连接的Hiredis上下文 */
    redisAsyncContext *pc; /* 命令连接的Hiredis上下文 */
    mstime_t cc_conn_time; /* cc连接的时间 */
    mstime_t pc_conn_time; /* pc连接的时间 */
    mstime_t pc_last_activity; /* 我们最后收到消息的时间 */
    mstime_t last_avail_time; /* 最后一次这个实例收到一个合法的PING回复的时间 */
    /* 正在等待回复的最后一次PING的时间。
     * 当收到回复时时设置为0
     * 当值为0并发送一个新的PING时设置为当前时间， */
    mstime_t act_ping_time;
    /* 最后一次发送ping的时间，仅用于防止在失败时发送过多的PING。空闲时间使用act_ping_time计算。 */
    mstime_t last_ping_time;
    /* 收到上一次回复的时间，无论收到的回复是什么。用来检查连接是否空闲，从而必须重连。 */
    mstime_t last_pong_time;
    mstime_t last_reconn_time;  /* 当连接段开始，最后一次企图重连的时间 */
} instanceLink;

typedef struct sentinelRedisInstance {
    int flags;      /* SRI_...标志 */
    char *name;     /* 从这个sentinel视角看到的master名 */
    char *runid;    /* 这个实例的runid或者这个sentinel的uniqueID */
    uint64_t config_epoch;  /* 配置纪元 */
    sentinelAddr *addr; /* Master host. */
    instanceLink *link; /* 到这个实例的连接，对于sentinels可能是共享的 */
    mstime_t last_pub_time;   /* 最后一次我们我们发送hello的时间 */
    mstime_t last_hello_time; /* 仅Sentinel使用，最后一次我们收到hello消息的时间 */
    mstime_t last_master_down_reply_time; /* 最后一次收到SENTINEL is-master-down的回复时间 */
    mstime_t s_down_since_time; /* Subjectively down since time. */
    mstime_t o_down_since_time; /* Objectively down since time. */
    mstime_t down_after_period; /* 如果经历了 Consider it down after that period. */
    mstime_t info_refresh;  /* 我们最后一次收到INFO回复的时间点 */
    dict *renamed_commands;     /* 重命名的命令 */

    /* 角色和我们第一次观察到变更为该角色的时间。
     * 在延迟替换时这是很有用的。我们需要等待一段时间来给新的leader报告新的配置。 */
    int role_reported;
    mstime_t role_reported_time;
    mstime_t slave_conf_change_time; /* 最后一次slave的master地址改变的时间点 */

    /* master相关 */
    dict *sentinels;    /* 监视相同的master的其他sentinels */
    dict *slaves;       /* 这个master的slaves */
    unsigned int quorum;/* 故障转移时需要统一的sentinels数量 */
    int parallel_syncs; /* 同时有多少slaves进行重配置。 */
    char *auth_pass;    /* 验证master和slaves时需要提供的密码 */

    /* slave相关 */
    mstime_t master_link_down_time; /* slave的复制连接下线的时间 */
    int slave_priority; /* 根据INFO输出的slave的优先级 */
    mstime_t slave_reconf_sent_time; /* 发送SLAVE OF <new>的时间 */
    struct sentinelRedisInstance *master; /* 如果是slave，这个字段表示她的master */
    char *slave_master_host;    /* INFO报告的master host */
    int slave_master_port;      /* INFO报告的master port */
    int slave_master_link_status; /* Master link status as reported by INFO */
    unsigned long long slave_repl_offset; /* Slave replication offset. */
    /* Failover */
    char *leader;       /* 如果是master，表示执行故障转移时的runid，如果是sentinel，表示这个sentinel投票的leader */
    uint64_t leader_epoch; /* leader的配置纪元 */
    uint64_t failover_epoch; /* 当前执行的故障转移的配置纪元 */
    int failover_state; /* See SENTINEL_FAILOVER_STATE_* defines. */
    mstime_t failover_state_change_time;
    mstime_t failover_start_time;   /* 最后一次故障转移企图开始的时间 */
    mstime_t failover_timeout;      /* 刷新故障转移状态的最大时间 */
    mstime_t failover_delay_logged; /* For what failover_start_time value we
                                       logged the failover delay. */
    struct sentinelRedisInstance *promoted_slave; /* 晋升的slave实例 */
    /* 用来提醒管理员和重新配置客户端的脚本：如果为NULL则没有脚本会执行 */
    char *notification_script;
    char *client_reconfig_script;
    sds info; /* cached INFO output */
} sentinelRedisInstance;

/* Sentinel状态 */
struct sentinelState {
    char myid[CONFIG_RUN_ID_SIZE + 1]; /* 这个Sentinel的ID */
    uint64_t current_epoch;         /* 当前纪元 */
    dict *masters;      /* 所有master的字典。key是实例名，value是sentinelRedisInstance指针 */
    int tilt;           /* 我们是否处于TILT模式? */
    int running_scripts;    /* 正在执行的脚本的数量 */
    mstime_t tilt_start_time;       /* TILT模式开始时间 */
    mstime_t previous_time;         /* 上次运行定时器函数的时间 */
    list *scripts_queue;            /* 等待执行的用户脚本队列 */
    char *announce_ip;  /* gossiped给其它sentinels的IP地址 */
    int announce_port;  /* gossiped给其它sentinels的IP端口 */
    unsigned long simfailure_flags; /* 模拟故障转移 */
    int deny_scripts_reconfig; /* 是否运行通过Allow SENTINEL SET ...在运行时改变脚本地址 */
} sentinel;

/* A script execution job. */
typedef struct sentinelScriptJob {
    int flags;              /* Script job flags: SENTINEL_SCRIPT_* */
    int retry_num;          /* Number of times we tried to execute it. */
    char **argv;            /* Arguments to call the script. */
    mstime_t start_time;    /* Script execution time if the script is running,
                               otherwise 0 if we are allowed to retry the
                               execution at any time. If the script is not
                               running and it's not 0, it means: do not run
                               before the specified time. */
    pid_t pid;              /* Script execution pid. */
} sentinelScriptJob;

/* ======================= hiredis ae.c adapters =============================
/* 注意：这个实现取自hiredis/adapters/ae.h，然而我们copy后做了一些修改为了：
 * 1) 使用我们自己的分配器
 * 2) 拥有对适配器的完全控制权 */

typedef struct redisAeEvents {
    redisAsyncContext *context;
    aeEventLoop *loop;
    int fd;
    int reading, writing;
} redisAeEvents;

static void redisAeReadEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void) el);
    ((void) fd);
    ((void) mask);

    redisAeEvents *e = (redisAeEvents *) privdata;
    redisAsyncHandleRead(e->context);
}

static void redisAeWriteEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void) el);
    ((void) fd);
    ((void) mask);

    redisAeEvents *e = (redisAeEvents *) privdata;
    redisAsyncHandleWrite(e->context);
}

static void redisAeAddRead(void *privdata) {
    redisAeEvents *e = (redisAeEvents *) privdata;
    aeEventLoop *loop = e->loop;
    if (!e->reading) {
        e->reading = 1;
        aeCreateFileEvent(loop, e->fd, AE_READABLE, redisAeReadEvent, e);
    }
}

static void redisAeDelRead(void *privdata) {
    redisAeEvents *e = (redisAeEvents *) privdata;
    aeEventLoop *loop = e->loop;
    if (e->reading) {
        e->reading = 0;
        aeDeleteFileEvent(loop, e->fd, AE_READABLE);
    }
}

static void redisAeAddWrite(void *privdata) {
    redisAeEvents *e = (redisAeEvents *) privdata;
    aeEventLoop *loop = e->loop;
    if (!e->writing) {
        e->writing = 1;
        aeCreateFileEvent(loop, e->fd, AE_WRITABLE, redisAeWriteEvent, e);
    }
}

static void redisAeDelWrite(void *privdata) {
    redisAeEvents *e = (redisAeEvents *) privdata;
    aeEventLoop *loop = e->loop;
    if (e->writing) {
        e->writing = 0;
        aeDeleteFileEvent(loop, e->fd, AE_WRITABLE);
    }
}

static void redisAeCleanup(void *privdata) {
    redisAeEvents *e = (redisAeEvents *) privdata;
    redisAeDelRead(privdata);
    redisAeDelWrite(privdata);
    zfree(e);
}

static int redisAeAttach(aeEventLoop *loop, redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    redisAeEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return C_ERR;

    /* Create container for context and r/w events */
    e = (redisAeEvents *) zmalloc(sizeof(*e));
    e->context = ac;
    e->loop = loop;
    e->fd = c->fd;
    e->reading = e->writing = 0;

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = redisAeAddRead;
    ac->ev.delRead = redisAeDelRead;
    ac->ev.addWrite = redisAeAddWrite;
    ac->ev.delWrite = redisAeDelWrite;
    ac->ev.cleanup = redisAeCleanup;
    ac->ev.data = e;

    return C_OK;
}

/* ============================= Prototypes ================================= */

void sentinelLinkEstablishedCallback(const redisAsyncContext *c, int status);

void sentinelDisconnectCallback(const redisAsyncContext *c, int status);

void sentinelReceiveHelloMessages(redisAsyncContext *c, void *reply, void *privdata);

sentinelRedisInstance *sentinelGetMasterByName(char *name);

char *sentinelGetSubjectiveLeader(sentinelRedisInstance *master);

char *sentinelGetObjectiveLeader(sentinelRedisInstance *master);

int yesnotoi(char *s);

void instanceLinkConnectionError(const redisAsyncContext *c);

const char *sentinelRedisInstanceTypeStr(sentinelRedisInstance *ri);

void sentinelAbortFailover(sentinelRedisInstance *ri);

void sentinelEvent(int level, char *type, sentinelRedisInstance *ri, const char *fmt, ...);

sentinelRedisInstance *sentinelSelectSlave(sentinelRedisInstance *master);

void sentinelScheduleScriptExecution(char *path, ...);

void sentinelStartFailover(sentinelRedisInstance *master);

void sentinelDiscardReplyCallback(redisAsyncContext *c, void *reply, void *privdata);

int sentinelSendSlaveOf(sentinelRedisInstance *ri, char *host, int port);

char *sentinelVoteLeader(sentinelRedisInstance *master, uint64_t req_epoch, char *req_runid, uint64_t *leader_epoch);

void sentinelFlushConfig(void);

void sentinelGenerateInitialMonitorEvents(void);

int sentinelSendPing(sentinelRedisInstance *ri);

int sentinelForceHelloUpdateForMaster(sentinelRedisInstance *master);

sentinelRedisInstance *getSentinelRedisInstanceByAddrAndRunID(dict *instances, char *ip, int port, char *runid);

void sentinelSimFailureCrash(void);

/* ========================= Dictionary types =============================== */

uint64_t dictSdsHash(const void *key);

uint64_t dictSdsCaseHash(const void *key);

int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);

int dictSdsKeyCaseCompare(void *privdata, const void *key1, const void *key2);

void releaseSentinelRedisInstance(sentinelRedisInstance *ri);

void dictInstancesValDestructor(void *privdata, void *obj) {
    UNUSED(privdata);
    releaseSentinelRedisInstance(obj);
}

/* Instance name (sds) -> instance (sentinelRedisInstance pointer)
 *
 * also used for: sentinelRedisInstance->sentinels dictionary that maps
 * sentinels ip:port to last seen time in Pub/Sub hello message. */
dictType instancesDictType = {
        dictSdsHash,               /* hash function */
        NULL,                      /* key dup */
        NULL,                      /* val dup */
        dictSdsKeyCompare,         /* key compare */
        NULL,                      /* key destructor */
        dictInstancesValDestructor /* val destructor */
};

/* Instance runid (sds) -> votes (long casted to void*)
 *
 * This is useful into sentinelGetObjectiveLeader() function in order to
 * count the votes and understand who is the leader. */
dictType leaderVotesDictType = {
        dictSdsHash,               /* hash function */
        NULL,                      /* key dup */
        NULL,                      /* val dup */
        dictSdsKeyCompare,         /* key compare */
        NULL,                      /* key destructor */
        NULL                       /* val destructor */
};

/* Instance renamed commands table. */
dictType renamedCommandsDictType = {
        dictSdsCaseHash,           /* hash function */
        NULL,                      /* key dup */
        NULL,                      /* val dup */
        dictSdsKeyCaseCompare,     /* key compare */
        dictSdsDestructor,         /* key destructor */
        dictSdsDestructor          /* val destructor */
};

/* =========================== Initialization =============================== */

void sentinelCommand(client *c);

void sentinelInfoCommand(client *c);

void sentinelSetCommand(client *c);

void sentinelPublishCommand(client *c);

void sentinelRoleCommand(client *c);

struct redisCommand sentinelcmds[] = {
        {"ping",         pingCommand,            1,  "",   0, NULL, 0, 0, 0, 0, 0},
        {"sentinel",     sentinelCommand,        -2, "",   0, NULL, 0, 0, 0, 0, 0},
        {"subscribe",    subscribeCommand,       -2, "",   0, NULL, 0, 0, 0, 0, 0},
        {"unsubscribe",  unsubscribeCommand,     -1, "",   0, NULL, 0, 0, 0, 0, 0},
        {"psubscribe",   psubscribeCommand,      -2, "",   0, NULL, 0, 0, 0, 0, 0},
        {"punsubscribe", punsubscribeCommand,    -1, "",   0, NULL, 0, 0, 0, 0, 0},
        {"publish",      sentinelPublishCommand, 3,  "",   0, NULL, 0, 0, 0, 0, 0},
        {"info",         sentinelInfoCommand,    -1, "",   0, NULL, 0, 0, 0, 0, 0},
        {"role",         sentinelRoleCommand,    1,  "l",  0, NULL, 0, 0, 0, 0, 0},
        {"client",       clientCommand,          -2, "rs", 0, NULL, 0, 0, 0, 0, 0},
        {"shutdown",     shutdownCommand,        -1, "",   0, NULL, 0, 0, 0, 0, 0}
};

/* 使用Sentinel配置重写Redis配置：现在仅重写端口号 */
void initSentinelConfig(void) {
    server.port = REDIS_SENTINEL_PORT;
}

/* 执行Sentinel模式初始化 */
void initSentinel(void) {
    unsigned int j;

    /* 切换命令表：移除通用的Redis命令，增加SENTINEL命令 */
    dictEmpty(server.commands, NULL);
    for (j = 0; j < sizeof(sentinelcmds) / sizeof(sentinelcmds[0]); j++) {
        int retval;
        struct redisCommand *cmd = sentinelcmds + j;

        retval = dictAdd(server.commands, sdsnew(cmd->name), cmd);
        serverAssert(retval == DICT_OK);
    }

    /* 初始化Sentinel结构体 */
    sentinel.current_epoch = 0;
    sentinel.masters = dictCreate(&instancesDictType, NULL);
    sentinel.tilt = 0;
    sentinel.tilt_start_time = 0;
    sentinel.previous_time = mstime();
    sentinel.running_scripts = 0;
    sentinel.scripts_queue = listCreate();
    sentinel.announce_ip = NULL;
    sentinel.announce_port = 0;
    sentinel.simfailure_flags = SENTINEL_SIMFAILURE_NONE;
    sentinel.deny_scripts_reconfig = SENTINEL_DEFAULT_DENY_SCRIPTS_RECONFIG;
    memset(sentinel.myid, 0, sizeof(sentinel.myid));
}

/* 这个函数会在server运行在Sentinel模式下时调用，来启动、加载配置和为正常操作做准备 */
void sentinelIsRunning(void) {
    int j;

    if (server.configfile == NULL) {
        serverLog(LL_WARNING,
                  "Sentinel started without a config file. Exiting...");
        exit(1);
    } else if (access(server.configfile, W_OK) == -1) {
        serverLog(LL_WARNING,
                  "Sentinel config file %s is not writable: %s. Exiting...",
                  server.configfile, strerror(errno));
        exit(1);
    }

    /* 如果Sentinel在配置文件中还没有一个ID，我们将会随机生成一个并持久化到磁盘上。
     * 从现在开始，即使重启也会使用同一个ID */
    for (j = 0; j < CONFIG_RUN_ID_SIZE; j++)
        if (sentinel.myid[j] != 0) break;

    if (j == CONFIG_RUN_ID_SIZE) {
        /* 选择一个ID并持久化到配置文件中 */
        getRandomHexChars(sentinel.myid, CONFIG_RUN_ID_SIZE);
        sentinelFlushConfig();
    }

    /* Log its ID to make debugging of issues simpler. */
    serverLog(LL_WARNING, "Sentinel ID is %s", sentinel.myid);

    /* 在启动时，对于每个配置的master，我们想要生成一个+monitor的事件 */
    sentinelGenerateInitialMonitorEvents();
}

/* ============================== sentinelAddr ============================== */

/* Create a sentinelAddr object and return it on success.
 * On error NULL is returned and errno is set to:
 *  ENOENT: Can't resolve the hostname.
 *  EINVAL: Invalid port number.
 */
sentinelAddr *createSentinelAddr(char *hostname, int port) {
    char ip[NET_IP_STR_LEN];
    sentinelAddr *sa;

    if (port < 0 || port > 65535) {
        errno = EINVAL;
        return NULL;
    }
    if (anetResolve(NULL, hostname, ip, sizeof(ip)) == ANET_ERR) {
        errno = ENOENT;
        return NULL;
    }
    sa = zmalloc(sizeof(*sa));
    sa->ip = sdsnew(ip);
    sa->port = port;
    return sa;
}

/* Return a duplicate of the source address. */
sentinelAddr *dupSentinelAddr(sentinelAddr *src) {
    sentinelAddr *sa;

    sa = zmalloc(sizeof(*sa));
    sa->ip = sdsnew(src->ip);
    sa->port = src->port;
    return sa;
}

/* Free a Sentinel address. Can't fail. */
void releaseSentinelAddr(sentinelAddr *sa) {
    sdsfree(sa->ip);
    zfree(sa);
}

/* Return non-zero if two addresses are equal. */
int sentinelAddrIsEqual(sentinelAddr *a, sentinelAddr *b) {
    return a->port == b->port && !strcasecmp(a->ip, b->ip);
}

/* =========================== Events notification ========================== */

/* 发送一个event到log，订阅频道和用户通知脚本中。
 * level是logging日志的等级，仅LL_WARNING会触发用户通知脚本。
 * type是消息类型，也用于发布信息的频道名。
 * ri是这个事件要应用的Redis实例，包含有用户通知脚本的路径。
 * 剩下的参数是printf格式的，如果fmt以%@开头，则ri参数不能为NULL，message将会以
 * <instance type> <instance name> <ip> <port>开头，如果ri不是master，将会附加上
 * @ <master name> <master ip> <master port>
 * */
void sentinelEvent(int level, char *type, sentinelRedisInstance *ri,
                   const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];
    robj *channel, *payload;

    /* 处理%@ */
    if (fmt[0] == '%' && fmt[1] == '@') {
        sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ?
                                        NULL : ri->master;

        if (master) {
            snprintf(msg, sizeof(msg), "%s %s %s %d @ %s %s %d",
                     sentinelRedisInstanceTypeStr(ri),
                     ri->name, ri->addr->ip, ri->addr->port,
                     master->name, master->addr->ip, master->addr->port);
        } else {
            snprintf(msg, sizeof(msg), "%s %s %s %d",
                     sentinelRedisInstanceTypeStr(ri),
                     ri->name, ri->addr->ip, ri->addr->port);
        }
        fmt += 2;
    } else {
        msg[0] = '\0';
    }

    /* Use vsprintf for the rest of the formatting if any. */
    if (fmt[0] != '\0') {
        va_start(ap, fmt);
        vsnprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), fmt, ap);
        va_end(ap);
    }

    /* Log the message if the log level allows it to be logged. */
    if (level >= server.verbosity)
        serverLog(level, "%s %s", type, msg);

    /* 如果不是debug级别，就发布到频道中 */
    if (level != LL_DEBUG) {
        channel = createStringObject(type, strlen(type));
        payload = createStringObject(msg, strlen(msg));
        pubsubPublishMessage(channel, payload);
        decrRefCount(channel);
        decrRefCount(payload);
    }

    /* Call the notification script if applicable. */
    if (level == LL_WARNING && ri != NULL) {
        sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ?
                                        ri : ri->master;
        if (master && master->notification_script) {
            sentinelScheduleScriptExecution(master->notification_script,
                                            type, msg, NULL);
        }
    }
}

/* This function is called only at startup and is used to generate a
 * +monitor event for every configured master. The same events are also
 * generated when a master to monitor is added at runtime via the
 * SENTINEL MONITOR command. */
void sentinelGenerateInitialMonitorEvents(void) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(sentinel.masters);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        sentinelEvent(LL_WARNING, "+monitor", ri, "%@ quorum %d", ri->quorum);
    }
    dictReleaseIterator(di);
}

/* ============================ script execution ============================ */

/* Release a script job structure and all the associated data. */
void sentinelReleaseScriptJob(sentinelScriptJob *sj) {
    int j = 0;

    while (sj->argv[j]) sdsfree(sj->argv[j++]);
    zfree(sj->argv);
    zfree(sj);
}

#define SENTINEL_SCRIPT_MAX_ARGS 16

void sentinelScheduleScriptExecution(char *path, ...) {
    va_list ap;
    char *argv[SENTINEL_SCRIPT_MAX_ARGS + 1];
    int argc = 1;
    sentinelScriptJob *sj;

    va_start(ap, path);
    while (argc < SENTINEL_SCRIPT_MAX_ARGS) {
        argv[argc] = va_arg(ap, char * );
        if (!argv[argc]) break;
        argv[argc] = sdsnew(argv[argc]); /* Copy the string. */
        argc++;
    }
    va_end(ap);
    argv[0] = sdsnew(path);

    sj = zmalloc(sizeof(*sj));
    sj->flags = SENTINEL_SCRIPT_NONE;
    sj->retry_num = 0;
    sj->argv = zmalloc(sizeof(char *) * (argc + 1));
    sj->start_time = 0;
    sj->pid = 0;
    memcpy(sj->argv, argv, sizeof(char *) * (argc + 1));

    listAddNodeTail(sentinel.scripts_queue, sj);

    /* Remove the oldest non running script if we already hit the limit. */
    if (listLength(sentinel.scripts_queue) > SENTINEL_SCRIPT_MAX_QUEUE) {
        listNode *ln;
        listIter li;

        listRewind(sentinel.scripts_queue, &li);
        while ((ln = listNext(&li)) != NULL) {
            sj = ln->value;

            if (sj->flags & SENTINEL_SCRIPT_RUNNING) continue;
            /* The first node is the oldest as we add on tail. */
            listDelNode(sentinel.scripts_queue, ln);
            sentinelReleaseScriptJob(sj);
            break;
        }
        serverAssert(listLength(sentinel.scripts_queue) <=
                     SENTINEL_SCRIPT_MAX_QUEUE);
    }
}

/* Lookup a script in the scripts queue via pid, and returns the list node
 * (so that we can easily remove it from the queue if needed). */
listNode *sentinelGetScriptListNodeByPid(pid_t pid) {
    listNode *ln;
    listIter li;

    listRewind(sentinel.scripts_queue, &li);
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;

        if ((sj->flags & SENTINEL_SCRIPT_RUNNING) && sj->pid == pid)
            return ln;
    }
    return NULL;
}

/* Run pending scripts if we are not already at max number of running
 * scripts. */
void sentinelRunPendingScripts(void) {
    listNode *ln;
    listIter li;
    mstime_t now = mstime();

    /* Find jobs that are not running and run them, from the top to the
     * tail of the queue, so we run older jobs first. */
    listRewind(sentinel.scripts_queue, &li);
    while (sentinel.running_scripts < SENTINEL_SCRIPT_MAX_RUNNING &&
           (ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;
        pid_t pid;

        /* Skip if already running. */
        if (sj->flags & SENTINEL_SCRIPT_RUNNING) continue;

        /* Skip if it's a retry, but not enough time has elapsed. */
        if (sj->start_time && sj->start_time > now) continue;

        sj->flags |= SENTINEL_SCRIPT_RUNNING;
        sj->start_time = mstime();
        sj->retry_num++;
        pid = fork();

        if (pid == -1) {
            /* Parent (fork error).
             * We report fork errors as signal 99, in order to unify the
             * reporting with other kind of errors. */
            sentinelEvent(LL_WARNING, "-script-error", NULL,
                          "%s %d %d", sj->argv[0], 99, 0);
            sj->flags &= ~SENTINEL_SCRIPT_RUNNING;
            sj->pid = 0;
        } else if (pid == 0) {
            /* Child */
            execve(sj->argv[0], sj->argv, environ);
            /* If we are here an error occurred. */
            _exit(2); /* Don't retry execution. */
        } else {
            sentinel.running_scripts++;
            sj->pid = pid;
            sentinelEvent(LL_DEBUG, "+script-child", NULL, "%ld", (long) pid);
        }
    }
}

/* How much to delay the execution of a script that we need to retry after
 * an error?
 *
 * We double the retry delay for every further retry we do. So for instance
 * if RETRY_DELAY is set to 30 seconds and the max number of retries is 10
 * starting from the second attempt to execute the script the delays are:
 * 30 sec, 60 sec, 2 min, 4 min, 8 min, 16 min, 32 min, 64 min, 128 min. */
mstime_t sentinelScriptRetryDelay(int retry_num) {
    mstime_t delay = SENTINEL_SCRIPT_RETRY_DELAY;

    while (retry_num-- > 1) delay *= 2;
    return delay;
}

/* Check for scripts that terminated, and remove them from the queue if the
 * script terminated successfully. If instead the script was terminated by
 * a signal, or returned exit code "1", it is scheduled to run again if
 * the max number of retries did not already elapsed. */
void sentinelCollectTerminatedScripts(void) {
    int statloc;
    pid_t pid;

    while ((pid = wait3(&statloc, WNOHANG, NULL)) > 0) {
        int exitcode = WEXITSTATUS(statloc);
        int bysignal = 0;
        listNode *ln;
        sentinelScriptJob *sj;

        if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);
        sentinelEvent(LL_DEBUG, "-script-child", NULL, "%ld %d %d",
                      (long) pid, exitcode, bysignal);

        ln = sentinelGetScriptListNodeByPid(pid);
        if (ln == NULL) {
            serverLog(LL_WARNING, "wait3() returned a pid (%ld) we can't find in our scripts execution queue!",
                      (long) pid);
            continue;
        }
        sj = ln->value;

        /* If the script was terminated by a signal or returns an
         * exit code of "1" (that means: please retry), we reschedule it
         * if the max number of retries is not already reached. */
        if ((bysignal || exitcode == 1) &&
            sj->retry_num != SENTINEL_SCRIPT_MAX_RETRY) {
            sj->flags &= ~SENTINEL_SCRIPT_RUNNING;
            sj->pid = 0;
            sj->start_time = mstime() +
                             sentinelScriptRetryDelay(sj->retry_num);
        } else {
            /* Otherwise let's remove the script, but log the event if the
             * execution did not terminated in the best of the ways. */
            if (bysignal || exitcode != 0) {
                sentinelEvent(LL_WARNING, "-script-error", NULL,
                              "%s %d %d", sj->argv[0], bysignal, exitcode);
            }
            listDelNode(sentinel.scripts_queue, ln);
            sentinelReleaseScriptJob(sj);
            sentinel.running_scripts--;
        }
    }
}

/* Kill scripts in timeout, they'll be collected by the
 * sentinelCollectTerminatedScripts() function. */
void sentinelKillTimedoutScripts(void) {
    listNode *ln;
    listIter li;
    mstime_t now = mstime();

    listRewind(sentinel.scripts_queue, &li);
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;

        if (sj->flags & SENTINEL_SCRIPT_RUNNING &&
            (now - sj->start_time) > SENTINEL_SCRIPT_MAX_RUNTIME) {
            sentinelEvent(LL_WARNING, "-script-timeout", NULL, "%s %ld",
                          sj->argv[0], (long) sj->pid);
            kill(sj->pid, SIGKILL);
        }
    }
}

/* Implements SENTINEL PENDING-SCRIPTS command. */
void sentinelPendingScriptsCommand(client *c) {
    listNode *ln;
    listIter li;

    addReplyMultiBulkLen(c, listLength(sentinel.scripts_queue));
    listRewind(sentinel.scripts_queue, &li);
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;
        int j = 0;

        addReplyMultiBulkLen(c, 10);

        addReplyBulkCString(c, "argv");
        while (sj->argv[j]) j++;
        addReplyMultiBulkLen(c, j);
        j = 0;
        while (sj->argv[j]) addReplyBulkCString(c, sj->argv[j++]);

        addReplyBulkCString(c, "flags");
        addReplyBulkCString(c,
                            (sj->flags & SENTINEL_SCRIPT_RUNNING) ? "running" : "scheduled");

        addReplyBulkCString(c, "pid");
        addReplyBulkLongLong(c, sj->pid);

        if (sj->flags & SENTINEL_SCRIPT_RUNNING) {
            addReplyBulkCString(c, "run-time");
            addReplyBulkLongLong(c, mstime() - sj->start_time);
        } else {
            mstime_t delay = sj->start_time ? (sj->start_time - mstime()) : 0;
            if (delay < 0) delay = 0;
            addReplyBulkCString(c, "run-delay");
            addReplyBulkLongLong(c, delay);
        }

        addReplyBulkCString(c, "retry-num");
        addReplyBulkLongLong(c, sj->retry_num);
    }
}

/* This function calls, if any, the client reconfiguration script with the
 * following parameters:
 *
 * <master-name> <role> <state> <from-ip> <from-port> <to-ip> <to-port>
 *
 * It is called every time a failover is performed.
 *
 * <state> is currently always "failover".
 * <role> is either "leader" or "observer".
 *
 * from/to fields are respectively master -> promoted slave addresses for
 * "start" and "end". */
/*  */
void sentinelCallClientReconfScript(sentinelRedisInstance *master, int role, char *state, sentinelAddr *from,
                                    sentinelAddr *to) {
    char fromport[32], toport[32];

    if (master->client_reconfig_script == NULL) return;
    ll2string(fromport, sizeof(fromport), from->port);
    ll2string(toport, sizeof(toport), to->port);
    sentinelScheduleScriptExecution(master->client_reconfig_script,
                                    master->name,
                                    (role == SENTINEL_LEADER) ? "leader" : "observer",
                                    state, from->ip, fromport, to->ip, toport, NULL);
}

/* =============================== instanceLink ============================= */

/* Create a not yet connected link object. */
instanceLink *createInstanceLink(void) {
    instanceLink *link = zmalloc(sizeof(*link));

    link->refcount = 1;
    link->disconnected = 1;
    link->pending_commands = 0;
    link->cc = NULL;
    link->pc = NULL;
    link->cc_conn_time = 0;
    link->pc_conn_time = 0;
    link->last_reconn_time = 0;
    link->pc_last_activity = 0;
    /* We set the act_ping_time to "now" even if we actually don't have yet
     * a connection with the node, nor we sent a ping.
     * This is useful to detect a timeout in case we'll not be able to connect
     * with the node at all. */
    link->act_ping_time = mstime();
    link->last_ping_time = 0;
    link->last_avail_time = mstime();
    link->last_pong_time = mstime();
    return link;
}

/* 在instanceLink上下文中断开一个hiredis连接 */
void instanceLinkCloseConnection(instanceLink *link, redisAsyncContext *c) {
    if (c == NULL) return;

    if (link->cc == c) {
        link->cc = NULL;
        link->pending_commands = 0;
    }
    if (link->pc == c) link->pc = NULL;
    c->data = NULL;
    link->disconnected = 1;
    redisAsyncFree(c);
}

/* Decrement the refcount of a link object, if it drops to zero, actually
 * free it and return NULL. Otherwise don't do anything and return the pointer
 * to the object.
 *
 * If we are not going to free the link and ri is not NULL, we rebind all the
 * pending requests in link->cc (hiredis connection for commands) to a
 * callback that will just ignore them. This is useful to avoid processing
 * replies for an instance that no longer exists. */
instanceLink *releaseInstanceLink(instanceLink *link, sentinelRedisInstance *ri) {
    serverAssert(link->refcount > 0);
    link->refcount--;
    if (link->refcount != 0) {
        if (ri && ri->link->cc) {
            /* This instance may have pending callbacks in the hiredis async
             * context, having as 'privdata' the instance that we are going to
             * free. Let's rewrite the callback list, directly exploiting
             * hiredis internal data structures, in order to bind them with
             * a callback that will ignore the reply at all. */
            redisCallback *cb;
            redisCallbackList *callbacks = &link->cc->replies;

            cb = callbacks->head;
            while (cb) {
                if (cb->privdata == ri) {
                    cb->fn = sentinelDiscardReplyCallback;
                    cb->privdata = NULL; /* Not strictly needed. */
                }
                cb = cb->next;
            }
        }
        return link; /* Other active users. */
    }

    instanceLinkCloseConnection(link, link->cc);
    instanceLinkCloseConnection(link, link->pc);
    zfree(link);
    return NULL;
}

/* This function will attempt to share the instance link we already have
 * for the same Sentinel in the context of a different master, with the
 * instance we are passing as argument.
 *
 * This way multiple Sentinel objects that refer all to the same physical
 * Sentinel instance but in the context of different masters will use
 * a single connection, will send a single PING per second for failure
 * detection and so forth.
 *
 * Return C_OK if a matching Sentinel was found in the context of a
 * different master and sharing was performed. Otherwise C_ERR
 * is returned. */
int sentinelTryConnectionSharing(sentinelRedisInstance *ri) {
    serverAssert(ri->flags & SRI_SENTINEL);
    dictIterator *di;
    dictEntry *de;

    if (ri->runid == NULL) return C_ERR; /* No way to identify it. */
    if (ri->link->refcount > 1) return C_ERR; /* Already shared. */

    di = dictGetIterator(sentinel.masters);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *master = dictGetVal(de), *match;
        /* We want to share with the same physical Sentinel referenced
         * in other masters, so skip our master. */
        if (master == ri->master) continue;
        match = getSentinelRedisInstanceByAddrAndRunID(master->sentinels,
                                                       NULL, 0, ri->runid);
        if (match == NULL) continue; /* No match. */
        if (match == ri) continue; /* Should never happen but... safer. */

        /* We identified a matching Sentinel, great! Let's free our link
         * and use the one of the matching Sentinel. */
        releaseInstanceLink(ri->link, NULL);
        ri->link = match->link;
        match->link->refcount++;
        return C_OK;
    }
    dictReleaseIterator(di);
    return C_ERR;
}

/* 当我们检测到一个sentinel切换了地址: 报告了一个不同的(ip,port)在hello消息中时，
 * 我们更新其他所有由当前sentinel监控的master的上下文，并且断连连接，在下一个定时器周期中将会重新连接 */
int sentinelUpdateSentinelAddressInAllMasters(sentinelRedisInstance *ri) {
    serverAssert(ri->flags & SRI_SENTINEL);
    dictIterator *di;
    dictEntry *de;
    int reconfigured = 0;

    di = dictGetIterator(sentinel.masters);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *master = dictGetVal(de), *match;
        match = getSentinelRedisInstanceByAddrAndRunID(master->sentinels,
                                                       NULL, 0, ri->runid);
        /* 如果没有匹配，说明这个复制群并不知道这个sentinel，检查下一个 */
        if (match == NULL) continue;

        /* 断开连接 */
        if (match->link->cc != NULL)
            instanceLinkCloseConnection(match->link, match->link->cc);
        if (match->link->pc != NULL)
            instanceLinkCloseConnection(match->link, match->link->pc);

        if (match == ri) continue; /* 地址已经更新 */

        /* 通过拷贝已经更新的sentinel的地址信息来更新当前的sentinel */
        releaseSentinelAddr(match->addr);
        match->addr = dupSentinelAddr(ri->addr);
        reconfigured++;
    }
    dictReleaseIterator(di);
    if (reconfigured)
        sentinelEvent(LL_NOTICE, "+sentinel-address-update", ri,
                      "%@ %d additional matching instances", reconfigured);
    return reconfigured;
}

/* This function is called when an hiredis connection reported an error.
 * We set it to NULL and mark the link as disconnected so that it will be
 * reconnected again.
 *
 * Note: we don't free the hiredis context as hiredis will do it for us
 * for async connections. */
void instanceLinkConnectionError(const redisAsyncContext *c) {
    instanceLink *link = c->data;
    int pubsub;

    if (!link) return;

    pubsub = (link->pc == c);
    if (pubsub)
        link->pc = NULL;
    else
        link->cc = NULL;
    link->disconnected = 1;
}

/* Hiredis connection established / disconnected callbacks. We need them
 * just to cleanup our link state. */
void sentinelLinkEstablishedCallback(const redisAsyncContext *c, int status) {
    if (status != C_OK) instanceLinkConnectionError(c);
}

void sentinelDisconnectCallback(const redisAsyncContext *c, int status) {
    UNUSED(status);
    instanceLinkConnectionError(c);
}

/* ========================== sentinelRedisInstance ========================= */

/* 创建一个Redis Instance，以下field必须要被客户端填充：
 * runid: 如果设置为null将会被之后的INFO命令的回复填充
 * info_refresh： 设置为0意味着我们未接受过INFO信息
 * 如果SRI_MASTER被设置将会加入到master字典中
 * 如果SRI_SLAVE/SRI_SENTINEL被设置则master参数不能为NULL并且这个实例会被加入到slaves/sentinels字典中
 * 如果这个实例是slave或sentinel，name参数将会被忽略，并且会被创建为hostname:port
 * 如果hostname无法被解析或者port超出返回，函数会失败，返回NULL并且errno将会被createSentinelAddr设置
 * 如果有了相同名字的master/相同地址的slave/相同ID的sentinel，这个函数也会失败，并设置errno=EBUSY
 * */
sentinelRedisInstance *createSentinelRedisInstance(char *name, int flags, char *hostname, int port, int quorum,
                                                   sentinelRedisInstance *master) {
    sentinelRedisInstance *ri;
    sentinelAddr *addr;
    dict *table = NULL;
    char slavename[NET_PEER_ID_LEN], *sdsname;

    serverAssert(flags & (SRI_MASTER | SRI_SLAVE | SRI_SENTINEL));
    serverAssert((flags & SRI_MASTER) || master != NULL);

    /* 检查地址合法性 */
    addr = createSentinelAddr(hostname, port);
    if (addr == NULL) return NULL;

    /* 对于slave，使用ip:port作为名字 */
    if (flags & SRI_SLAVE) {
        anetFormatAddr(slavename, sizeof(slavename), hostname, port);
        name = slavename;
    }

    /* 确保entry不会重重复 */
    if (flags & SRI_MASTER) table = sentinel.masters;
    else if (flags & SRI_SLAVE) table = master->slaves;
    else if (flags & SRI_SENTINEL) table = master->sentinels;
    sdsname = sdsnew(name);
    if (dictFind(table, sdsname)) {
        releaseSentinelAddr(addr);
        sdsfree(sdsname);
        errno = EBUSY;
        return NULL;
    }

    /* 创建instance object. */
    ri = zmalloc(sizeof(*ri));
    /* 注意：所有的实例在开始时都是断开状态，事件循环会负责连接它们 */
    ri->flags = flags;
    ri->name = sdsname;
    ri->runid = NULL;
    ri->config_epoch = 0;
    ri->addr = addr;
    ri->link = createInstanceLink();
    ri->last_pub_time = mstime();
    ri->last_hello_time = mstime();
    ri->last_master_down_reply_time = mstime();
    ri->s_down_since_time = 0;
    ri->o_down_since_time = 0;
    ri->down_after_period = master ? master->down_after_period :
                            SENTINEL_DEFAULT_DOWN_AFTER;
    ri->master_link_down_time = 0;
    ri->auth_pass = NULL;
    ri->slave_priority = SENTINEL_DEFAULT_SLAVE_PRIORITY;
    ri->slave_reconf_sent_time = 0;
    ri->slave_master_host = NULL;
    ri->slave_master_port = 0;
    ri->slave_master_link_status = SENTINEL_MASTER_LINK_STATUS_DOWN;
    ri->slave_repl_offset = 0;
    ri->sentinels = dictCreate(&instancesDictType, NULL);
    ri->quorum = quorum;
    ri->parallel_syncs = SENTINEL_DEFAULT_PARALLEL_SYNCS;
    ri->master = master;
    ri->slaves = dictCreate(&instancesDictType, NULL);
    ri->info_refresh = 0;
    ri->renamed_commands = dictCreate(&renamedCommandsDictType, NULL);

    /* Failover state. */
    ri->leader = NULL;
    ri->leader_epoch = 0;
    ri->failover_epoch = 0;
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = 0;
    ri->failover_start_time = 0;
    ri->failover_timeout = SENTINEL_DEFAULT_FAILOVER_TIMEOUT;
    ri->failover_delay_logged = 0;
    ri->promoted_slave = NULL;
    ri->notification_script = NULL;
    ri->client_reconfig_script = NULL;
    ri->info = NULL;

    /* Role */
    ri->role_reported = ri->flags & (SRI_MASTER | SRI_SLAVE);
    ri->role_reported_time = mstime();
    ri->slave_conf_change_time = mstime();

    /* Add into the right table. */
    dictAdd(table, ri->name, ri);
    return ri;
}

/* Release this instance and all its slaves, sentinels, hiredis connections.
 * This function does not take care of unlinking the instance from the main
 * masters table (if it is a master) or from its master sentinels/slaves table
 * if it is a slave or sentinel. */
void releaseSentinelRedisInstance(sentinelRedisInstance *ri) {
    /* Release all its slaves or sentinels if any. */
    dictRelease(ri->sentinels);
    dictRelease(ri->slaves);

    /* Disconnect the instance. */
    releaseInstanceLink(ri->link, ri);

    /* Free other resources. */
    sdsfree(ri->name);
    sdsfree(ri->runid);
    sdsfree(ri->notification_script);
    sdsfree(ri->client_reconfig_script);
    sdsfree(ri->slave_master_host);
    sdsfree(ri->leader);
    sdsfree(ri->auth_pass);
    sdsfree(ri->info);
    releaseSentinelAddr(ri->addr);
    dictRelease(ri->renamed_commands);

    /* Clear state into the master if needed. */
    if ((ri->flags & SRI_SLAVE) && (ri->flags & SRI_PROMOTED) && ri->master)
        ri->master->promoted_slave = NULL;

    zfree(ri);
}

/* Lookup a slave in a master Redis instance, by ip and port. */
sentinelRedisInstance *sentinelRedisInstanceLookupSlave(
        sentinelRedisInstance *ri, char *ip, int port) {
    sds key;
    sentinelRedisInstance *slave;
    char buf[NET_PEER_ID_LEN];

    serverAssert(ri->flags & SRI_MASTER);
    anetFormatAddr(buf, sizeof(buf), ip, port);
    key = sdsnew(buf);
    slave = dictFetchValue(ri->slaves, key);
    sdsfree(key);
    return slave;
}

/* Return the name of the type of the instance as a string. */
const char *sentinelRedisInstanceTypeStr(sentinelRedisInstance *ri) {
    if (ri->flags & SRI_MASTER) return "master";
    else if (ri->flags & SRI_SLAVE) return "slave";
    else if (ri->flags & SRI_SENTINEL) return "sentinel";
    else return "unknown";
}

/* 这个函数将会从指定的master中移除给定runid的sentinel。
 * 用于sentinel地址切换，移除旧的sentinel并增加一个相同runid的新地址的sentinel。
 * 如果移除成功返回1，否则返回0 */
int removeMatchingSentinelFromMaster(sentinelRedisInstance *master, char *runid) {
    dictIterator *di;
    dictEntry *de;
    int removed = 0;

    if (runid == NULL) return 0;

    di = dictGetSafeIterator(master->sentinels);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        if (ri->runid && strcmp(ri->runid, runid) == 0) {
            dictDelete(master->sentinels, ri->name);
            removed++;
        }
    }
    dictReleaseIterator(di);
    return removed;
}

/* 查找具有相同runid、ip和port的实例，没找到则返回NULL
 * runid或者ip可以为空。在这种情况下，仅会查找非null的域。 */
sentinelRedisInstance *getSentinelRedisInstanceByAddrAndRunID(dict *instances, char *ip, int port, char *runid) {
    dictIterator *di;
    dictEntry *de;
    sentinelRedisInstance *instance = NULL;

    serverAssert(ip || runid);   /* 至少有一个域不为空 */
    di = dictGetIterator(instances);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        if (runid && !ri->runid) continue;
        if ((runid == NULL || strcmp(ri->runid, runid) == 0) &&
            (ip == NULL || (strcmp(ri->addr->ip, ip) == 0 &&
                            ri->addr->port == port))) {
            instance = ri;
            break;
        }
    }
    dictReleaseIterator(di);
    return instance;
}

/* Master lookup by name */
sentinelRedisInstance *sentinelGetMasterByName(char *name) {
    sentinelRedisInstance *ri;
    sds sdsname = sdsnew(name);

    ri = dictFetchValue(sentinel.masters, sdsname);
    sdsfree(sdsname);
    return ri;
}

/* 增加指定的标志位到指定的dict中的所有实例中 */
void sentinelAddFlagsToDictOfRedisInstances(dict *instances, int flags) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        ri->flags |= flags;
    }
    dictReleaseIterator(di);
}

/* 移除指定的标志位从指定的dict中的所有实例中 */
void sentinelDelFlagsToDictOfRedisInstances(dict *instances, int flags) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        ri->flags &= ~flags;
    }
    dictReleaseIterator(di);
}

/* Reset the state of a monitored master:
 * 1) Remove all slaves.
 * 2) Remove all sentinels.
 * 3) Remove most of the flags resulting from runtime operations.
 * 4) Reset timers to their default value. For example after a reset it will be
 *    possible to failover again the same master ASAP, without waiting the
 *    failover timeout delay.
 * 5) In the process of doing this undo the failover if in progress.
 * 6) Disconnect the connections with the master (will reconnect automatically).
 */

#define SENTINEL_RESET_NO_SENTINELS (1<<0)

void sentinelResetMaster(sentinelRedisInstance *ri, int flags) {
    serverAssert(ri->flags & SRI_MASTER);
    dictRelease(ri->slaves);
    ri->slaves = dictCreate(&instancesDictType, NULL);
    if (!(flags & SENTINEL_RESET_NO_SENTINELS)) {
        dictRelease(ri->sentinels);
        ri->sentinels = dictCreate(&instancesDictType, NULL);
    }
    instanceLinkCloseConnection(ri->link, ri->link->cc);
    instanceLinkCloseConnection(ri->link, ri->link->pc);
    ri->flags &= SRI_MASTER;
    if (ri->leader) {
        sdsfree(ri->leader);
        ri->leader = NULL;
    }
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = 0;
    ri->failover_start_time = 0; /* We can failover again ASAP. */
    ri->promoted_slave = NULL;
    sdsfree(ri->runid);
    sdsfree(ri->slave_master_host);
    ri->runid = NULL;
    ri->slave_master_host = NULL;
    ri->link->act_ping_time = mstime();
    ri->link->last_ping_time = 0;
    ri->link->last_avail_time = mstime();
    ri->link->last_pong_time = mstime();
    ri->role_reported_time = mstime();
    ri->role_reported = SRI_MASTER;
    if (flags & SENTINEL_GENERATE_EVENT)
        sentinelEvent(LL_WARNING, "+reset-master", ri, "%@");
}

/* Call sentinelResetMaster() on every master with a name matching the specified
 * pattern. */
int sentinelResetMastersByPattern(char *pattern, int flags) {
    dictIterator *di;
    dictEntry *de;
    int reset = 0;

    di = dictGetIterator(sentinel.masters);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        if (ri->name) {
            if (stringmatch(pattern, ri->name, 0)) {
                sentinelResetMaster(ri, flags);
                reset++;
            }
        }
    }
    dictReleaseIterator(di);
    return reset;
}

/* 使用sentinelResetMaster()重置指定的master，并且修改ip:port，但是不修改name。
 * 这个用来处理+switch-master的事件。
 * 如果地址无法解析，返回C_ERR，否则返回C_OK。*/
int sentinelResetMasterAndChangeAddress(sentinelRedisInstance *master, char *ip, int port) {
    sentinelAddr *oldaddr, *newaddr;
    sentinelAddr **slaves = NULL;
    int numslaves = 0, j;
    dictIterator *di;
    dictEntry *de;

    newaddr = createSentinelAddr(ip, port);
    if (newaddr == NULL) return C_ERR;

    /* Make a list of slaves to add back after the reset.
     * Don't include the one having the address we are switching to. */
    di = dictGetIterator(master->slaves);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);

        if (sentinelAddrIsEqual(slave->addr, newaddr)) continue;
        slaves = zrealloc(slaves, sizeof(sentinelAddr *) * (numslaves + 1));
        slaves[numslaves++] = createSentinelAddr(slave->addr->ip,
                                                 slave->addr->port);
    }
    dictReleaseIterator(di);

    /* If we are switching to a different address, include the old address
     * as a slave as well, so that we'll be able to sense / reconfigure
     * the old master. */
    if (!sentinelAddrIsEqual(newaddr, master->addr)) {
        slaves = zrealloc(slaves, sizeof(sentinelAddr *) * (numslaves + 1));
        slaves[numslaves++] = createSentinelAddr(master->addr->ip,
                                                 master->addr->port);
    }

    /* Reset and switch address. */
    sentinelResetMaster(master, SENTINEL_RESET_NO_SENTINELS);
    oldaddr = master->addr;
    master->addr = newaddr;
    master->o_down_since_time = 0;
    master->s_down_since_time = 0;

    /* Add slaves back. */
    for (j = 0; j < numslaves; j++) {
        sentinelRedisInstance *slave;

        slave = createSentinelRedisInstance(NULL, SRI_SLAVE, slaves[j]->ip,
                                            slaves[j]->port, master->quorum, master);
        releaseSentinelAddr(slaves[j]);
        if (slave) sentinelEvent(LL_NOTICE, "+slave", slave, "%@");
    }
    zfree(slaves);

    /* Release the old address at the end so we are safe even if the function
     * gets the master->addr->ip and master->addr->port as arguments. */
    releaseSentinelAddr(oldaddr);
    sentinelFlushConfig();
    return C_OK;
}

/* 如果在ms内没有关于客观下线和主观下线的错误，则返回非0 */
int sentinelRedisInstanceNoDownFor(sentinelRedisInstance *ri, mstime_t ms) {
    mstime_t most_recent;

    most_recent = ri->s_down_since_time;
    if (ri->o_down_since_time > most_recent)
        most_recent = ri->o_down_since_time;
    return most_recent == 0 || (mstime() - most_recent) > ms;
}

/* Return the current master address, that is, its address or the address
 * of the promoted slave if already operational. */
sentinelAddr *sentinelGetCurrentMasterAddress(sentinelRedisInstance *master) {
    /* If we are failing over the master, and the state is already
     * SENTINEL_FAILOVER_STATE_RECONF_SLAVES or greater, it means that we
     * already have the new configuration epoch in the master, and the
     * slave acknowledged the configuration switch. Advertise the new
     * address. */
    if ((master->flags & SRI_FAILOVER_IN_PROGRESS) &&
        master->promoted_slave &&
        master->failover_state >= SENTINEL_FAILOVER_STATE_RECONF_SLAVES) {
        return master->promoted_slave->addr;
    } else {
        return master->addr;
    }
}

/* This function sets the down_after_period field value in 'master' to all
 * the slaves and sentinel instances connected to this master. */
void sentinelPropagateDownAfterPeriod(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    int j;
    dict *d[] = {master->slaves, master->sentinels, NULL};

    for (j = 0; d[j]; j++) {
        di = dictGetIterator(d[j]);
        while ((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
            ri->down_after_period = master->down_after_period;
        }
        dictReleaseIterator(di);
    }
}

char *sentinelGetInstanceTypeString(sentinelRedisInstance *ri) {
    if (ri->flags & SRI_MASTER) return "master";
    else if (ri->flags & SRI_SLAVE) return "slave";
    else if (ri->flags & SRI_SENTINEL) return "sentinel";
    else return "unknown";
}

/* 这个函数用来向Redis实例发送命令：从Sentinel发出的命令可能被重命名，比如master的CONFIG和SLAVEOF
 * 可能为了安全而被重命名。 在这种情况下，我们将会检查renamed_command表，并且转换成我们应该发送的命令 */
char *sentinelInstanceMapCommand(sentinelRedisInstance *ri, char *command) {
    sds sc = sdsnew(command);
    if (ri->master) ri = ri->master;
    char *retval = dictFetchValue(ri->renamed_commands, sc);
    sdsfree(sc);
    return retval ? retval : command;
}

/* ============================ Config handling ============================= */
/* 解析Sentinel相关配置 */
char *sentinelHandleConfiguration(char **argv, int argc) {
    sentinelRedisInstance *ri;

    if (!strcasecmp(argv[0], "monitor") && argc == 5) {
        /* monitor <name> <host> <port> <quorum> */
        int quorum = atoi(argv[4]);

        if (quorum <= 0) return "Quorum must be 1 or greater.";
        if (createSentinelRedisInstance(argv[1], SRI_MASTER, argv[2],
                                        atoi(argv[3]), quorum, NULL) == NULL) {
            switch (errno) {
                case EBUSY:
                    return "Duplicated master name.";
                case ENOENT:
                    return "Can't resolve master instance hostname.";
                case EINVAL:
                    return "Invalid port number";
            }
        }
    } else if (!strcasecmp(argv[0], "down-after-milliseconds") && argc == 3) {
        /* down-after-milliseconds <name> <milliseconds> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->down_after_period = atoi(argv[2]);
        if (ri->down_after_period <= 0)
            return "negative or zero time parameter.";
        sentinelPropagateDownAfterPeriod(ri);
    } else if (!strcasecmp(argv[0], "failover-timeout") && argc == 3) {
        /* failover-timeout <name> <milliseconds> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->failover_timeout = atoi(argv[2]);
        if (ri->failover_timeout <= 0)
            return "negative or zero time parameter.";
    } else if (!strcasecmp(argv[0], "parallel-syncs") && argc == 3) {
        /* parallel-syncs <name> <milliseconds> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->parallel_syncs = atoi(argv[2]);
    } else if (!strcasecmp(argv[0], "notification-script") && argc == 3) {
        /* notification-script <name> <path> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        if (access(argv[2], X_OK) == -1)
            return "Notification script seems non existing or non executable.";
        ri->notification_script = sdsnew(argv[2]);
    } else if (!strcasecmp(argv[0], "client-reconfig-script") && argc == 3) {
        /* client-reconfig-script <name> <path> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        if (access(argv[2], X_OK) == -1)
            return "Client reconfiguration script seems non existing or "
                   "non executable.";
        ri->client_reconfig_script = sdsnew(argv[2]);
    } else if (!strcasecmp(argv[0], "auth-pass") && argc == 3) {
        /* auth-pass <name> <password> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->auth_pass = sdsnew(argv[2]);
    } else if (!strcasecmp(argv[0], "current-epoch") && argc == 2) {
        /* current-epoch <epoch> */
        unsigned long long current_epoch = strtoull(argv[1], NULL, 10);
        if (current_epoch > sentinel.current_epoch)
            sentinel.current_epoch = current_epoch;
    } else if (!strcasecmp(argv[0], "myid") && argc == 2) {
        if (strlen(argv[1]) != CONFIG_RUN_ID_SIZE)
            return "Malformed Sentinel id in myid option.";
        memcpy(sentinel.myid, argv[1], CONFIG_RUN_ID_SIZE);
    } else if (!strcasecmp(argv[0], "config-epoch") && argc == 3) {
        /* config-epoch <name> <epoch> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->config_epoch = strtoull(argv[2], NULL, 10);
        /* The following update of current_epoch is not really useful as
         * now the current epoch is persisted on the config file, but
         * we leave this check here for redundancy. */
        if (ri->config_epoch > sentinel.current_epoch)
            sentinel.current_epoch = ri->config_epoch;
    } else if (!strcasecmp(argv[0], "leader-epoch") && argc == 3) {
        /* leader-epoch <name> <epoch> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->leader_epoch = strtoull(argv[2], NULL, 10);
    } else if ((!strcasecmp(argv[0], "known-slave") ||
                !strcasecmp(argv[0], "known-replica")) && argc == 4) {
        sentinelRedisInstance *slave;

        /* known-replica <name> <ip> <port> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        if ((slave = createSentinelRedisInstance(NULL, SRI_SLAVE, argv[2],
                                                 atoi(argv[3]), ri->quorum, ri)) == NULL) {
            return "Wrong hostname or port for replica.";
        }
    } else if (!strcasecmp(argv[0], "known-sentinel") &&
               (argc == 4 || argc == 5)) {
        sentinelRedisInstance *si;

        if (argc == 5) { /* Ignore the old form without runid. */
            /* known-sentinel <name> <ip> <port> [runid] */
            ri = sentinelGetMasterByName(argv[1]);
            if (!ri) return "No such master with specified name.";
            if ((si = createSentinelRedisInstance(argv[4], SRI_SENTINEL, argv[2],
                                                  atoi(argv[3]), ri->quorum, ri)) == NULL) {
                return "Wrong hostname or port for sentinel.";
            }
            si->runid = sdsnew(argv[4]);
            sentinelTryConnectionSharing(si);
        }
    } else if (!strcasecmp(argv[0], "rename-command") && argc == 4) {
        /* rename-command <name> <command> <renamed-command> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        sds oldcmd = sdsnew(argv[2]);
        sds newcmd = sdsnew(argv[3]);
        if (dictAdd(ri->renamed_commands, oldcmd, newcmd) != DICT_OK) {
            sdsfree(oldcmd);
            sdsfree(newcmd);
            return "Same command renamed multiple times with rename-command.";
        }
    } else if (!strcasecmp(argv[0], "announce-ip") && argc == 2) {
        /* announce-ip <ip-address> */
        if (strlen(argv[1]))
            sentinel.announce_ip = sdsnew(argv[1]);
    } else if (!strcasecmp(argv[0], "announce-port") && argc == 2) {
        /* announce-port <port> */
        sentinel.announce_port = atoi(argv[1]);
    } else if (!strcasecmp(argv[0], "deny-scripts-reconfig") && argc == 2) {
        /* deny-scripts-reconfig <yes|no> */
        if ((sentinel.deny_scripts_reconfig = yesnotoi(argv[1])) == -1) {
            return "Please specify yes or no for the "
                   "deny-scripts-reconfig options.";
        }
    } else {
        return "Unrecognized sentinel configuration statement.";
    }
    return NULL;
}

/* Implements CONFIG REWRITE for "sentinel" option.
 * This is used not just to rewrite the configuration given by the user
 * (the configured masters) but also in order to retain the state of
 * Sentinel across restarts: config epoch of masters, associated slaves
 * and sentinel instances, and so forth. */
void rewriteConfigSentinelOption(struct rewriteConfigState *state) {
    dictIterator *di, *di2;
    dictEntry *de;
    sds line;

    /* sentinel unique ID. */
    line = sdscatprintf(sdsempty(), "sentinel myid %s", sentinel.myid);
    rewriteConfigRewriteLine(state, "sentinel", line, 1);

    /* sentinel deny-scripts-reconfig. */
    line = sdscatprintf(sdsempty(), "sentinel deny-scripts-reconfig %s",
                        sentinel.deny_scripts_reconfig ? "yes" : "no");
    rewriteConfigRewriteLine(state, "sentinel", line,
                             sentinel.deny_scripts_reconfig != SENTINEL_DEFAULT_DENY_SCRIPTS_RECONFIG);

    /* For every master emit a "sentinel monitor" config entry. */
    di = dictGetIterator(sentinel.masters);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *master, *ri;
        sentinelAddr *master_addr;

        /* sentinel monitor */
        master = dictGetVal(de);
        master_addr = sentinelGetCurrentMasterAddress(master);
        line = sdscatprintf(sdsempty(), "sentinel monitor %s %s %d %d",
                            master->name, master_addr->ip, master_addr->port,
                            master->quorum);
        rewriteConfigRewriteLine(state, "sentinel", line, 1);

        /* sentinel down-after-milliseconds */
        if (master->down_after_period != SENTINEL_DEFAULT_DOWN_AFTER) {
            line = sdscatprintf(sdsempty(),
                                "sentinel down-after-milliseconds %s %ld",
                                master->name, (long) master->down_after_period);
            rewriteConfigRewriteLine(state, "sentinel", line, 1);
        }

        /* sentinel failover-timeout */
        if (master->failover_timeout != SENTINEL_DEFAULT_FAILOVER_TIMEOUT) {
            line = sdscatprintf(sdsempty(),
                                "sentinel failover-timeout %s %ld",
                                master->name, (long) master->failover_timeout);
            rewriteConfigRewriteLine(state, "sentinel", line, 1);
        }

        /* sentinel parallel-syncs */
        if (master->parallel_syncs != SENTINEL_DEFAULT_PARALLEL_SYNCS) {
            line = sdscatprintf(sdsempty(),
                                "sentinel parallel-syncs %s %d",
                                master->name, master->parallel_syncs);
            rewriteConfigRewriteLine(state, "sentinel", line, 1);
        }

        /* sentinel notification-script */
        if (master->notification_script) {
            line = sdscatprintf(sdsempty(),
                                "sentinel notification-script %s %s",
                                master->name, master->notification_script);
            rewriteConfigRewriteLine(state, "sentinel", line, 1);
        }

        /* sentinel client-reconfig-script */
        if (master->client_reconfig_script) {
            line = sdscatprintf(sdsempty(),
                                "sentinel client-reconfig-script %s %s",
                                master->name, master->client_reconfig_script);
            rewriteConfigRewriteLine(state, "sentinel", line, 1);
        }

        /* sentinel auth-pass */
        if (master->auth_pass) {
            line = sdscatprintf(sdsempty(),
                                "sentinel auth-pass %s %s",
                                master->name, master->auth_pass);
            rewriteConfigRewriteLine(state, "sentinel", line, 1);
        }

        /* sentinel config-epoch */
        line = sdscatprintf(sdsempty(),
                            "sentinel config-epoch %s %llu",
                            master->name, (unsigned long long) master->config_epoch);
        rewriteConfigRewriteLine(state, "sentinel", line, 1);

        /* sentinel leader-epoch */
        line = sdscatprintf(sdsempty(),
                            "sentinel leader-epoch %s %llu",
                            master->name, (unsigned long long) master->leader_epoch);
        rewriteConfigRewriteLine(state, "sentinel", line, 1);

        /* sentinel known-slave */
        di2 = dictGetIterator(master->slaves);
        while ((de = dictNext(di2)) != NULL) {
            sentinelAddr *slave_addr;

            ri = dictGetVal(de);
            slave_addr = ri->addr;

            /* If master_addr (obtained using sentinelGetCurrentMasterAddress()
             * so it may be the address of the promoted slave) is equal to this
             * slave's address, a failover is in progress and the slave was
             * already successfully promoted. So as the address of this slave
             * we use the old master address instead. */
            if (sentinelAddrIsEqual(slave_addr, master_addr))
                slave_addr = master->addr;
            line = sdscatprintf(sdsempty(),
                                "sentinel known-replica %s %s %d",
                                master->name, slave_addr->ip, slave_addr->port);
            rewriteConfigRewriteLine(state, "sentinel", line, 1);
        }
        dictReleaseIterator(di2);

        /* sentinel known-sentinel */
        di2 = dictGetIterator(master->sentinels);
        while ((de = dictNext(di2)) != NULL) {
            ri = dictGetVal(de);
            if (ri->runid == NULL) continue;
            line = sdscatprintf(sdsempty(),
                                "sentinel known-sentinel %s %s %d %s",
                                master->name, ri->addr->ip, ri->addr->port, ri->runid);
            rewriteConfigRewriteLine(state, "sentinel", line, 1);
        }
        dictReleaseIterator(di2);

        /* sentinel rename-command */
        di2 = dictGetIterator(master->renamed_commands);
        while ((de = dictNext(di2)) != NULL) {
            sds oldname = dictGetKey(de);
            sds newname = dictGetVal(de);
            line = sdscatprintf(sdsempty(),
                                "sentinel rename-command %s %s %s",
                                master->name, oldname, newname);
            rewriteConfigRewriteLine(state, "sentinel", line, 1);
        }
        dictReleaseIterator(di2);
    }

    /* sentinel current-epoch is a global state valid for all the masters. */
    line = sdscatprintf(sdsempty(),
                        "sentinel current-epoch %llu", (unsigned long long) sentinel.current_epoch);
    rewriteConfigRewriteLine(state, "sentinel", line, 1);

    /* sentinel announce-ip. */
    if (sentinel.announce_ip) {
        line = sdsnew("sentinel announce-ip ");
        line = sdscatrepr(line, sentinel.announce_ip, sdslen(sentinel.announce_ip));
        rewriteConfigRewriteLine(state, "sentinel", line, 1);
    }

    /* sentinel announce-port. */
    if (sentinel.announce_port) {
        line = sdscatprintf(sdsempty(), "sentinel announce-port %d",
                            sentinel.announce_port);
        rewriteConfigRewriteLine(state, "sentinel", line, 1);
    }

    dictReleaseIterator(di);
}

/* This function uses the config rewriting Redis engine in order to persist
 * the state of the Sentinel in the current configuration file.
 *
 * Before returning the function calls fsync() against the generated
 * configuration file to make sure changes are committed to disk.
 *
 * On failure the function logs a warning on the Redis log. */
void sentinelFlushConfig(void) {
    int fd = -1;
    int saved_hz = server.hz;
    int rewrite_status;

    server.hz = CONFIG_DEFAULT_HZ;
    rewrite_status = rewriteConfig(server.configfile);
    server.hz = saved_hz;

    if (rewrite_status == -1) goto werr;
    if ((fd = open(server.configfile, O_RDONLY)) == -1) goto werr;
    if (fsync(fd) == -1) goto werr;
    if (close(fd) == EOF) goto werr;
    return;

    werr:
    if (fd != -1) close(fd);
    serverLog(LL_WARNING, "WARNING: Sentinel was not able to save the new configuration on disk!!!: %s",
              strerror(errno));
}

/* ====================== hiredis connection handling ======================= */

/* Send the AUTH command with the specified master password if needed.
 * Note that for slaves the password set for the master is used.
 *
 * We don't check at all if the command was successfully transmitted
 * to the instance as if it fails Sentinel will detect the instance down,
 * will disconnect and reconnect the link and so forth. */
void sentinelSendAuthIfNeeded(sentinelRedisInstance *ri, redisAsyncContext *c) {
    char *auth_pass = (ri->flags & SRI_MASTER) ? ri->auth_pass :
                      ri->master->auth_pass;

    if (auth_pass) {
        if (redisAsyncCommand(c, sentinelDiscardReplyCallback, ri, "%s %s",
                              sentinelInstanceMapCommand(ri, "AUTH"),
                              auth_pass) == C_OK)
            ri->link->pending_commands++;
    }
}

/* Use CLIENT SETNAME to name the connection in the Redis instance as
 * sentinel-<first_8_chars_of_runid>-<connection_type>
 * The connection type is "cmd" or "pubsub" as specified by 'type'.
 *
 * This makes it possible to list all the sentinel instances connected
 * to a Redis servewr with CLIENT LIST, grepping for a specific name format. */
void sentinelSetClientName(sentinelRedisInstance *ri, redisAsyncContext *c, char *type) {
    char name[64];

    snprintf(name, sizeof(name), "sentinel-%.8s-%s", sentinel.myid, type);
    if (redisAsyncCommand(c, sentinelDiscardReplyCallback, ri,
                          "%s SETNAME %s",
                          sentinelInstanceMapCommand(ri, "CLIENT"),
                          name) == C_OK) {
        ri->link->pending_commands++;
    }
}

/* 如果ri的link是断连状态，创建一个异步的连接。
 * 注意：命令连接和订阅连接只要有一条断开，link->disconnected都会变成true */
void sentinelReconnectInstance(sentinelRedisInstance *ri) {
    if (ri->link->disconnected == 0) return;
    if (ri->addr->port == 0) return; /* 如果port == 0说明这个sentinel是非法的 */
    instanceLink *link = ri->link;
    mstime_t now = mstime();

    if (now - ri->link->last_reconn_time < SENTINEL_PING_PERIOD) return;
    ri->link->last_reconn_time = now;

    /* 命令连接 */
    if (link->cc == NULL) {
        // 发起一个异步连接
        link->cc = redisAsyncConnectBind(ri->addr->ip, ri->addr->port, NET_FIRST_BIND_ADDR);
        if (link->cc->err) {
            sentinelEvent(LL_DEBUG, "-cmd-link-reconnection", ri, "%@ #%s",
                          link->cc->errstr);
            instanceLinkCloseConnection(link, link->cc);
        } else {
            link->pending_commands = 0;
            link->cc_conn_time = mstime();
            link->cc->data = link;
            // 加入事件循环，创建Connect回调，Disconnect回调，发送AUTH消息，设置Client名字，发送PING消息
            redisAeAttach(server.el, link->cc);
            redisAsyncSetConnectCallback(link->cc,
                                         sentinelLinkEstablishedCallback);
            redisAsyncSetDisconnectCallback(link->cc,
                                            sentinelDisconnectCallback);
            sentinelSendAuthIfNeeded(ri, link->cc);
            sentinelSetClientName(ri, link->cc, "cmd");

            /* 当重连后我们尽快先发送一个PING命令 */
            sentinelSendPing(ri);
        }
    }
    /* 对于master和slave，发起订阅连接 */
    if ((ri->flags & (SRI_MASTER | SRI_SLAVE)) && link->pc == NULL) {
        link->pc = redisAsyncConnectBind(ri->addr->ip, ri->addr->port, NET_FIRST_BIND_ADDR);
        if (link->pc->err) {
            sentinelEvent(LL_DEBUG, "-pubsub-link-reconnection", ri, "%@ #%s",
                          link->pc->errstr);
            instanceLinkCloseConnection(link, link->pc);
        } else {
            int retval;

            link->pc_conn_time = mstime();
            link->pc->data = link;
            // 将pc socket附加到ae事件循环框架上
            redisAeAttach(server.el, link->pc);
            redisAsyncSetConnectCallback(link->pc,
                                         sentinelLinkEstablishedCallback);
            redisAsyncSetDisconnectCallback(link->pc,
                                            sentinelDisconnectCallback);
            sentinelSendAuthIfNeeded(ri, link->pc);
            sentinelSetClientName(ri, link->pc, "pubsub");
            /* 订阅__sentinel:hello频道 */
            retval = redisAsyncCommand(link->pc,
                                       sentinelReceiveHelloMessages, ri, "%s %s",
                                       sentinelInstanceMapCommand(ri, "SUBSCRIBE"),
                                       SENTINEL_HELLO_CHANNEL);
            if (retval != C_OK) {
                /* 如果我们订阅失败，我们就关闭连接重试 */
                instanceLinkCloseConnection(link, link->pc);
                return;
            }
        }
    }
    /* 清理disconnected状态 */
    if (link->cc && (ri->flags & SRI_SENTINEL || link->pc))
        link->disconnected = 0;
}

/* ======================== Redis instances pinging  ======================== */

/* 如果master看起来是正常的返回true：
 * 1) 在当前的配置中它是master
 * 2) 它自己报告说它是master
 * 3) 它不处于客观下线或者主观下线状态
 * 4) 我们获取最后的INFO不超过两个INFO周期 */
int sentinelMasterLooksSane(sentinelRedisInstance *master) {
    return
            master->flags & SRI_MASTER &&
            master->role_reported == SRI_MASTER &&
            (master->flags & (SRI_S_DOWN | SRI_O_DOWN)) == 0 &&
            (mstime() - master->info_refresh) < SENTINEL_INFO_PERIOD * 2;
}

/* 处理从master接收到的INFO命令的回复 */
void sentinelRefreshInstanceInfo(sentinelRedisInstance *ri, const char *info) {
    sds *lines;
    int numlines, j;
    int role = 0;

    /* 缓存INFO的回复信息 */
    sdsfree(ri->info);
    ri->info = sdsnew(info);

    /* 如果在INFO输出中没有下面的域，那么需要被设定为给定的值 */
    ri->master_link_down_time = 0;

    /* 按行解析 */
    lines = sdssplitlen(info, strlen(info), "\r\n", 2, &numlines);
    for (j = 0; j < numlines; j++) {
        sentinelRedisInstance *slave;
        sds l = lines[j];

        /* 查找runid：run_id:<40 hex chars>*/
        if (sdslen(l) >= 47 && !memcmp(l, "run_id:", 7)) {
            if (ri->runid == NULL) {        // 如果为空，则填充
                ri->runid = sdsnewlen(l + 7, 40);
            } else {                        // 如果不为空，说明该实例进行过重启
                if (strncmp(ri->runid, l + 7, 40) != 0) {
                    sentinelEvent(LL_NOTICE, "+reboot", ri, "%@");
                    sdsfree(ri->runid);
                    ri->runid = sdsnewlen(l + 7, 40);
                }
            }
        }

        /* old versions: slave0:<ip>,<port>,<state>
         * new versions: slave0:ip=127.0.0.1,port=9999,... */
        /* 如果我们是从master收到的slave信息，则新增slave */
        if ((ri->flags & SRI_MASTER) &&
            sdslen(l) >= 7 &&
            !memcmp(l, "slave", 5) && isdigit(l[5])) {
            char *ip, *port, *end;

            if (strstr(l, "ip=") == NULL) {
                /* Old format. */
                ip = strchr(l, ':');
                if (!ip) continue;
                ip++; /* Now ip points to start of ip address. */
                port = strchr(ip, ',');
                if (!port) continue;
                *port = '\0'; /* nul term for easy access. */
                port++; /* Now port points to start of port number. */
                end = strchr(port, ',');
                if (!end) continue;
                *end = '\0'; /* nul term for easy access. */
            } else {
                /* New format. */
                ip = strstr(l, "ip=");
                if (!ip) continue;
                ip += 3; /* Now ip points to start of ip address. */
                port = strstr(l, "port=");
                if (!port) continue;
                port += 5; /* Now port points to start of port number. */
                /* Nul term both fields for easy access. */
                end = strchr(ip, ',');
                if (end) *end = '\0';
                end = strchr(port, ',');
                if (end) *end = '\0';
            }

            /* 如果我们没有记录这个slave，则新增 */
            if (sentinelRedisInstanceLookupSlave(ri, ip, atoi(port)) == NULL) {
                if ((slave = createSentinelRedisInstance(NULL, SRI_SLAVE, ip,
                                                         atoi(port), ri->quorum, ri)) != NULL) {
                    sentinelEvent(LL_NOTICE, "+slave", slave, "%@");
                    sentinelFlushConfig();
                }
            }
        }

        /* master_link_down_since_seconds:<seconds> */
        /* 更新主从断开时间 */
        if (sdslen(l) >= 32 &&
            !memcmp(l, "master_link_down_since_seconds", 30)) {
            ri->master_link_down_time = strtoll(l + 31, NULL, 10) * 1000;
        }

        /* role:<role> */
        if (!memcmp(l, "role:master", 11)) role = SRI_MASTER;
        else if (!memcmp(l, "role:slave", 10)) role = SRI_SLAVE;

        // 如果当前角色是SLAVE，则更新slave相关的信息
        if (role == SRI_SLAVE) {
            /* master_host:<host> */
            if (sdslen(l) >= 12 && !memcmp(l, "master_host:", 12)) {
                if (ri->slave_master_host == NULL ||
                    strcasecmp(l + 12, ri->slave_master_host)) {
                    sdsfree(ri->slave_master_host);
                    ri->slave_master_host = sdsnew(l + 12);
                    ri->slave_conf_change_time = mstime();
                }
            }

            /* master_port:<port> */
            if (sdslen(l) >= 12 && !memcmp(l, "master_port:", 12)) {
                int slave_master_port = atoi(l + 12);

                if (ri->slave_master_port != slave_master_port) {
                    ri->slave_master_port = slave_master_port;
                    ri->slave_conf_change_time = mstime();
                }
            }

            /* master_link_status:<status> */
            if (sdslen(l) >= 19 && !memcmp(l, "master_link_status:", 19)) {
                ri->slave_master_link_status =
                        (strcasecmp(l + 19, "up") == 0) ?
                        SENTINEL_MASTER_LINK_STATUS_UP :
                        SENTINEL_MASTER_LINK_STATUS_DOWN;
            }

            /* slave_priority:<priority> */
            if (sdslen(l) >= 15 && !memcmp(l, "slave_priority:", 15))
                ri->slave_priority = atoi(l + 15);

            /* slave_repl_offset:<offset> */
            if (sdslen(l) >= 18 && !memcmp(l, "slave_repl_offset:", 18))
                ri->slave_repl_offset = strtoull(l + 18, NULL, 10);
        }
    }
    ri->info_refresh = mstime();
    sdsfreesplitres(lines, numlines);

    /* ---------------------------- Acting half -----------------------------
    /* 如果处于TILT模式，则只会记录相关信息，不执行某些操作 */

    /* 当上次报告的角色和本次报告的不一样时 */
    if (role != ri->role_reported) {
        ri->role_reported_time = mstime();
        ri->role_reported = role;
        if (role == SRI_SLAVE) ri->slave_conf_change_time = mstime();
        /* 如果本次汇报的角色配置和当前配置是一致的，我们记录+role-change事件，
         * 如果本次汇报的角色配置和当前配置不一致，我们记录-role-change事件。 */
        sentinelEvent(LL_VERBOSE,
                      ((ri->flags & (SRI_MASTER | SRI_SLAVE)) == role) ?
                      "+role-change" : "-role-change",
                      ri, "%@ new reported role is %s",
                      role == SRI_MASTER ? "master" : "slave",
                      ri->flags & SRI_MASTER ? "master" : "slave");
    }

    /* 下面的行为不能在TILT模式下执行 */
    if (sentinel.tilt) return;

    /* 处理 master -> slave 角色转变 */
    if ((ri->flags & SRI_MASTER) && role == SRI_SLAVE) {
        /* 我们什么都不做，但是一个声明为slave的master被sentinel认为是无法访问的，
         * 如果该实例一直这样报告，我们将会认为它是主观下线的，最终可能会触发一次故障转移。 */
    }

    /* 处理slave->master的角色转变 */
    if ((ri->flags & SRI_SLAVE) && role == SRI_MASTER) {
        /* 如果该slave是晋升的slave，我们需要修改故障转移状态机 */
        if ((ri->flags & SRI_PROMOTED) &&
            (ri->master->flags & SRI_FAILOVER_IN_PROGRESS) &&
            (ri->master->failover_state ==
             SENTINEL_FAILOVER_STATE_WAIT_PROMOTION)) {
            /* 注意：我们确认了slave已经重新配置为了master，所以我们把master的配置纪元作为当前纪元，
             * 我们是通过这个纪元赢得故障转移的选举的。
             * 这将会强制其他sentinels更新它们的配置（假设没有一个更新的纪元可用）。 */
            ri->master->config_epoch = ri->master->failover_epoch;
            ri->master->failover_state = SENTINEL_FAILOVER_STATE_RECONF_SLAVES;
            ri->master->failover_state_change_time = mstime();
            sentinelFlushConfig();
            sentinelEvent(LL_WARNING, "+promoted-slave", ri, "%@");
            if (sentinel.simfailure_flags &
                SENTINEL_SIMFAILURE_CRASH_AFTER_PROMOTION)
                sentinelSimFailureCrash();
            sentinelEvent(LL_WARNING, "+failover-state-reconf-slaves",
                          ri->master, "%@");
            sentinelCallClientReconfScript(ri->master, SENTINEL_LEADER,
                                           "start", ri->master->addr, ri->addr);
            sentinelForceHelloUpdateForMaster(ri->master);
        } else {
            /* 另外一个slave转化为了master。我们将原来master重新配置为slave。
             * 在此之前等待8s时间，来接收新的配置，减少数据包乱序带来的影响。 */
            mstime_t wait_time = SENTINEL_PUBLISH_PERIOD * 4;

            if (!(ri->flags & SRI_PROMOTED) &&
                sentinelMasterLooksSane(ri->master) &&
                sentinelRedisInstanceNoDownFor(ri, wait_time) &&
                mstime() - ri->role_reported_time > wait_time) {
                int retval = sentinelSendSlaveOf(ri,
                                                 ri->master->addr->ip,
                                                 ri->master->addr->port);
                if (retval == C_OK)
                    sentinelEvent(LL_NOTICE, "+convert-to-slave", ri, "%@");
            }
        }
    }

    /* slaves开始跟从一个新的master */
    if ((ri->flags & SRI_SLAVE) &&
        role == SRI_SLAVE &&
        (ri->slave_master_port != ri->master->addr->port ||
         strcasecmp(ri->slave_master_host, ri->master->addr->ip))) {
        mstime_t wait_time = ri->master->failover_timeout;

        /* 在更新slave之前确保master是正常的 */
        if (sentinelMasterLooksSane(ri->master) &&
            sentinelRedisInstanceNoDownFor(ri, wait_time) &&
            mstime() - ri->slave_conf_change_time > wait_time) {
            int retval = sentinelSendSlaveOf(ri,
                                             ri->master->addr->ip,
                                             ri->master->addr->port);
            if (retval == C_OK)
                sentinelEvent(LL_NOTICE, "+fix-slave-config", ri, "%@");
        }
    }

    /* 检查slave重配置的进度状态 */
    if ((ri->flags & SRI_SLAVE) && role == SRI_SLAVE &&
        (ri->flags & (SRI_RECONF_SENT | SRI_RECONF_INPROG))) {
        /* SRI_RECONF_SENT -> SRI_RECONF_INPROG. */
        if ((ri->flags & SRI_RECONF_SENT) &&
            ri->slave_master_host &&
            strcmp(ri->slave_master_host,
                   ri->master->promoted_slave->addr->ip) == 0 &&
            ri->slave_master_port == ri->master->promoted_slave->addr->port) {
            ri->flags &= ~SRI_RECONF_SENT;
            ri->flags |= SRI_RECONF_INPROG;
            sentinelEvent(LL_NOTICE, "+slave-reconf-inprog", ri, "%@");
        }

        /* SRI_RECONF_INPROG -> SRI_RECONF_DONE */
        if ((ri->flags & SRI_RECONF_INPROG) &&
            ri->slave_master_link_status == SENTINEL_MASTER_LINK_STATUS_UP) {
            ri->flags &= ~SRI_RECONF_INPROG;
            ri->flags |= SRI_RECONF_DONE;
            sentinelEvent(LL_NOTICE, "+slave-reconf-done", ri, "%@");
        }
    }
}

/* INFO命令回复的回调函数 */
void sentinelInfoReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = privdata;
    instanceLink *link = c->data;
    redisReply *r;

    if (!reply || !link) return;
    link->pending_commands--;
    r = reply;

    if (r->type == REDIS_REPLY_STRING)
        sentinelRefreshInstanceInfo(ri, r->str);
}

/* Just discard the reply. We use this when we are not monitoring the return
 * value of the command but its effects directly. */
void sentinelDiscardReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    instanceLink *link = c->data;
    UNUSED(reply);
    UNUSED(privdata);

    if (link) link->pending_commands--;
}

/* sentinel的PING命令的回调函数 */
void sentinelPingReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = privdata;
    instanceLink *link = c->data;
    redisReply *r;

    if (!reply || !link) return;
    link->pending_commands--;
    r = reply;

    if (r->type == REDIS_REPLY_STATUS ||
        r->type == REDIS_REPLY_ERROR) {
        /* 仅当命令可接受时才更新连接表示可用性的字段 */
        if (strncmp(r->str, "PONG", 4) == 0 ||
            strncmp(r->str, "LOADING", 7) == 0 ||
            strncmp(r->str, "MASTERDOWN", 10) == 0) {
            link->last_avail_time = mstime();
            link->act_ping_time = 0; /* 已收到最后一个PING的回复 */
        } else {
            /* Send a SCRIPT KILL command if the instance appears to be
             * down because of a busy script. */
            if (strncmp(r->str, "BUSY", 4) == 0 &&
                (ri->flags & SRI_S_DOWN) &&
                !(ri->flags & SRI_SCRIPT_KILL_SENT)) {
                if (redisAsyncCommand(ri->link->cc,
                                      sentinelDiscardReplyCallback, ri,
                                      "%s KILL",
                                      sentinelInstanceMapCommand(ri, "SCRIPT")) == C_OK) {
                    ri->link->pending_commands++;
                }
                ri->flags |= SRI_SCRIPT_KILL_SENT;
            }
        }
    }
    link->last_pong_time = mstime();
}

/* PUBLISH命令的回调函数：这个PUBLISH命令是我们发送给master来宣告自己的 */
void sentinelPublishReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = privdata;
    instanceLink *link = c->data;
    redisReply *r;

    if (!reply || !link) return;
    link->pending_commands--;
    r = reply;

    /* 更新我们last_pub_time时间。否则我们将会在100ms内重试 */
    if (r->type != REDIS_REPLY_ERROR)
        ri->last_pub_time = mstime();
}

/* 处理从master或者slave收到的__sentinel__:hello频道中的hello消息，或者从其它sentinel直接发来的信息。
 * 如果消息中指定的master name未知，消息将被丢弃。 */
void sentinelProcessHelloMessage(char *hello, int hello_len) {
    /* Format is composed of 8 tokens:
     * 0=ip,1=port,2=runid,3=current_epoch,4=master_name,
     * 5=master_ip,6=master_port,7=master_config_epoch. */
    int numtokens, port, removed, master_port;
    uint64_t current_epoch, master_config_epoch;
    char **token = sdssplitlen(hello, hello_len, ",", 1, &numtokens);
    sentinelRedisInstance *si, *master;

    if (numtokens == 8) {
        /* 包含一个master的引用 */
        master = sentinelGetMasterByName(token[4]);
        if (!master) goto cleanup; /* 未知的master，跳过 */

        /* 首先，检查我们是否有相同ip:port和runid的sentinel的信息 */
        port = atoi(token[1]);
        master_port = atoi(token[6]);
        si = getSentinelRedisInstanceByAddrAndRunID(
                master->sentinels, token[0], port, token[2]);
        current_epoch = strtoull(token[3], NULL, 10);
        master_config_epoch = strtoull(token[7], NULL, 10);

        /* Sentinel处理情况分析：
         * 1. 相同runid并且相同ip:port，什么都不必做
         * 2. 相同runid但是不同ip:port，说明这个sentinel出现了地址切换，删除，并重新添加
         * 3. 不同runid但是相同ip:port，说明这个ip:port所在的sentinel地址是非法的，我们需要标示所有具有该runid的sentinel非法，然后新增一个新的。
         * 4. 不同runid并且不同ip:port，直接新增。 */
        if (!si) {          // 如果没发现相同ip和runid的sentinel，说明这是一个新的sentinel
            /* 如果没有，因为sentinel的地址切换，我们需要移除所有相同runid的sentinels，
             * 我们将会在之后添加一个相同runid但是有新的地址的sentinel */
            removed = removeMatchingSentinelFromMaster(master, token[2]);
            if (removed) {
                // 如果找到并删除了相同runid但不同ip的sentinel，说明是sentinel进行了地址切换
                sentinelEvent(LL_NOTICE, "+sentinel-address-switch", master,
                              "%@ ip %s port %d for %s", token[0], port, token[2]);
            } else {
                /* 如果找到了相同ip:port但不同runid的sentinel，说明这个sentinel是非法的，
                 * 我们将把这个ip:port关联的sentinel标记为非法，我们将把port设置为0，来标示地址非法。
                 * 我们将会在之后收到带有该runid实例的Hello消息时更新。 */
                sentinelRedisInstance *other =
                        getSentinelRedisInstanceByAddrAndRunID(
                                master->sentinels, token[0], port, NULL);
                if (other) {
                    sentinelEvent(LL_NOTICE, "+sentinel-invalid-addr", other, "%@");
                    other->addr->port = 0; /* 这意味着：地址是非法的 */
                    sentinelUpdateSentinelAddressInAllMasters(other);
                }
            }

            /* 增加一个新的sentinel，并增加到master的sentinels中 */
            si = createSentinelRedisInstance(token[2], SRI_SENTINEL,
                                             token[0], port, master->quorum, master);

            if (si) {
                if (!removed) sentinelEvent(LL_NOTICE, "+sentinel", si, "%@");
                /* 刚创建完实例，runid为空，我们需要立即填充它，否则以后没机会了 */
                si->runid = sdsnew(token[2]);
                sentinelTryConnectionSharing(si);
                if (removed) sentinelUpdateSentinelAddressInAllMasters(si);
                sentinelFlushConfig();
            }
        }

        /* 如果当前纪元更大，我们更新本地纪元 */
        if (current_epoch > sentinel.current_epoch) {
            sentinel.current_epoch = current_epoch;
            sentinelFlushConfig();
            sentinelEvent(LL_WARNING, "+new-epoch", master, "%llu",
                          (unsigned long long) sentinel.current_epoch);
        }

        /* 如果接收到的配置更新，则更新master信息 */
        if (si && master->config_epoch < master_config_epoch) {
            master->config_epoch = master_config_epoch;
            if (master_port != master->addr->port ||
                strcmp(master->addr->ip, token[5])) {
                sentinelAddr *old_addr;

                sentinelEvent(LL_WARNING, "+config-update-from", si, "%@");
                sentinelEvent(LL_WARNING, "+switch-master",
                              master, "%s %s %d %s %d",
                              master->name,
                              master->addr->ip, master->addr->port,
                              token[5], master_port);

                old_addr = dupSentinelAddr(master->addr);
                sentinelResetMasterAndChangeAddress(master, token[5], master_port);
                sentinelCallClientReconfScript(master,
                                               SENTINEL_OBSERVER, "start",
                                               old_addr, master->addr);
                releaseSentinelAddr(old_addr);
            }
        }

        /* 更新sentinel的状态 */
        if (si) si->last_hello_time = mstime();
    }

    cleanup:
    sdsfreesplitres(token, numtokens);
}

/* __sentinel__:hello频道的回调，用来发现关注这个master的其他sentinel */
void sentinelReceiveHelloMessages(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = privdata;
    redisReply *r;
    UNUSED(c);

    if (!reply || !ri) return;
    r = reply;

    /* Update the last activity in the pubsub channel. Note that since we
     * receive our messages as well this timestamp can be used to detect
     * if the link is probably disconnected even if it seems otherwise. */
    /* 更新订阅连接的最后活跃时间。注意：我们收到消息的这个时间点可以用来做连接的活跃度检测。 */
    ri->link->pc_last_activity = mstime();

    /* 检查响应是否是我们期望的格式:
     * message
     * __sentinel__:hello"
     * {sentinel_ip,sentinel_port,sentinel_runid,current_epoch,master_name,master_ip,master_port,master_config_epoch}
     * */
    if (r->type != REDIS_REPLY_ARRAY ||
        r->elements != 3 ||
        r->element[0]->type != REDIS_REPLY_STRING ||
        r->element[1]->type != REDIS_REPLY_STRING ||
        r->element[2]->type != REDIS_REPLY_STRING ||
        strcmp(r->element[0]->str, "message") != 0)
        return;

    /* 如果消息中包含我们的id说明，说明是我们自己发布的信息，忽略 */
    if (strstr(r->element[2]->str, sentinel.myid) != NULL) return;

    sentinelProcessHelloMessage(r->element[2]->str, r->element[2]->len);
}

/* 通过订阅频道发送Hello信息到指定Redis实例，来广播复制群master的当前配置，
 * 并且同时宣告这个Sentinel的存在。
 * 消息格式：
 * {sentinel_ip,sentinel_port,sentinel_runid,current_epoch,master_name,master_ip,master_port,master_config_epoch}
 * 如果PUBLISH入队，返回C_OK，否则返回C_ERR */
int sentinelSendHello(sentinelRedisInstance *ri) {
    char ip[NET_IP_STR_LEN];
    char payload[NET_IP_STR_LEN + 1024];
    int retval;
    char *announce_ip;
    int announce_port;
    sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ? ri : ri->master;
    sentinelAddr *master_addr = sentinelGetCurrentMasterAddress(master);

    if (ri->link->disconnected) return C_ERR;

    /* 如果指定announce_ip，则使用announce_ip，否则使用我们自己的ip */
    if (sentinel.announce_ip) {
        announce_ip = sentinel.announce_ip;
    } else {
        if (anetSockName(ri->link->cc->c.fd, ip, sizeof(ip), NULL) == -1)
            return C_ERR;
        announce_ip = ip;
    }
    announce_port = sentinel.announce_port ?
                    sentinel.announce_port : server.port;

    /* 格式化并发送Hello消息 */
    snprintf(payload, sizeof(payload),
             "%s,%d,%s,%llu," /* 关于这个sentinel的信息 */
             "%s,%s,%d,%llu", /* 关于这个master的信息 */
             announce_ip, announce_port, sentinel.myid,
             (unsigned long long) sentinel.current_epoch,
            /* --- */
             master->name, master_addr->ip, master_addr->port,
             (unsigned long long) master->config_epoch);
    retval = redisAsyncCommand(ri->link->cc,
                               sentinelPublishReplyCallback, ri, "%s %s %s",
                               sentinelInstanceMapCommand(ri, "PUBLISH"),
                               SENTINEL_HELLO_CHANNEL, payload);
    if (retval != C_OK) return C_ERR;
    ri->link->pending_commands++;
    return C_OK;
}

/* Reset last_pub_time in all the instances in the specified dictionary
 * in order to force the delivery of an Hello update ASAP. */
void sentinelForceHelloUpdateDictOfRedisInstances(dict *instances) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(instances);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        if (ri->last_pub_time >= (SENTINEL_PUBLISH_PERIOD + 1))
            ri->last_pub_time -= (SENTINEL_PUBLISH_PERIOD + 1);
    }
    dictReleaseIterator(di);
}

/* 这个函数将会分发hello消息到master相关联所有的Redis和Sentinel实例。
 * 从技术上，这不是必要的，因为我们最终将会在SENTINEL_PUBLISH_PERIOD周期内发送每一个实例的信息。
 * 然后当一个sentinel更新配置时，立即发送给其它sentinel是一个很好的做法。 */
int sentinelForceHelloUpdateForMaster(sentinelRedisInstance *master) {
    if (!(master->flags & SRI_MASTER)) return C_ERR;
    if (master->last_pub_time >= (SENTINEL_PUBLISH_PERIOD + 1))
        master->last_pub_time -= (SENTINEL_PUBLISH_PERIOD + 1);
    sentinelForceHelloUpdateDictOfRedisInstances(master->sentinels);
    sentinelForceHelloUpdateDictOfRedisInstances(master->slaves);
    return C_OK;
}

/* 发送一个PING到指定的Redis实例并且如果act_ping_time==0, 刷新它的时间
 * act_ping_time==0意味着我们已经接收到之前PING的PONG回复。
 * 出错时返回0，并且我们不考虑PING入队的问题。*/
int sentinelSendPing(sentinelRedisInstance *ri) {
    int retval = redisAsyncCommand(ri->link->cc,
                                   sentinelPingReplyCallback, ri, "%s",
                                   sentinelInstanceMapCommand(ri, "PING"));
    if (retval == C_OK) {
        ri->link->pending_commands++;
        ri->link->last_ping_time = mstime();
        /* 如果当前我们没有在等待PONG，那么我更新act_ping_time表示我们正在等待一个PONG */
        if (ri->link->act_ping_time == 0)
            ri->link->act_ping_time = ri->link->last_ping_time;
        return 1;
    } else {
        return 0;
    }
}

/* 发送周期性的PING、INFO和PUBLISH命令到指定的Redis实例 */
void sentinelSendPeriodicCommands(sentinelRedisInstance *ri) {
    mstime_t now = mstime();
    mstime_t info_period, ping_period;
    int retval;

    /* 如果当前没有成功连接，直接返回 */
    if (ri->link->disconnected) return;

    /* 像INFO、PING、PUBLISH这样的命令并不重要，如果这个连接上阻塞的命令过多，我们直接返回。
     * 当网络环境不好时，我们不希望发送为此消耗过多的内存。
     * 注意我们有保护措施，如果这条连接检测到超时，连接会断开并重连。 */
    if (ri->link->pending_commands >=
        SENTINEL_MAX_PENDING_COMMANDS * ri->link->refcount)
        return;

    /* 当该实例是一个处于客观下线或者故障转移中的master的slave时，INFO的发送周期从10s一次改为1s一次。
     * 这样我们可以更快的捕捉到slave向master的晋升。
     * 类似的，如果这个slave和master断连了，我们也会更频繁的监视INFO的输出，来更快的捕捉到恢复。*/
    if ((ri->flags & SRI_SLAVE) &&
        ((ri->master->flags & (SRI_O_DOWN | SRI_FAILOVER_IN_PROGRESS)) ||
         (ri->master_link_down_time != 0))) {
        info_period = 1000;
    } else {
        info_period = SENTINEL_INFO_PERIOD;
    }

    /* ping的周期为min(down_after_period,SENTINEL_PING_PERIOD) */
    ping_period = ri->down_after_period;
    if (ping_period > SENTINEL_PING_PERIOD) ping_period = SENTINEL_PING_PERIOD;

    /* 对master和slaves发送INFO命令 */
    if ((ri->flags & SRI_SENTINEL) == 0 &&
        (ri->info_refresh == 0 ||
         (now - ri->info_refresh) > info_period)) {
        retval = redisAsyncCommand(ri->link->cc,
                                   sentinelInfoReplyCallback, ri, "%s",
                                   sentinelInstanceMapCommand(ri, "INFO"));
        if (retval == C_OK) ri->link->pending_commands++;
    }

    /* 向所有实例发送PING命令 */
    if ((now - ri->link->last_pong_time) > ping_period &&
        (now - ri->link->last_ping_time) > ping_period / 2) {
        sentinelSendPing(ri);
    }

    /* 每2s向所有实例发布PUBLISH hello消息:
     * 其中对于master和slaves通过频道发送，对sentinel通过PUBLISH发送 */
    if ((now - ri->last_pub_time) > SENTINEL_PUBLISH_PERIOD) {
        sentinelSendHello(ri);
    }
}

/* =========================== SENTINEL command ============================= */

const char *sentinelFailoverStateStr(int state) {
    switch (state) {
        case SENTINEL_FAILOVER_STATE_NONE:
            return "none";
        case SENTINEL_FAILOVER_STATE_WAIT_START:
            return "wait_start";
        case SENTINEL_FAILOVER_STATE_SELECT_SLAVE:
            return "select_slave";
        case SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE:
            return "send_slaveof_noone";
        case SENTINEL_FAILOVER_STATE_WAIT_PROMOTION:
            return "wait_promotion";
        case SENTINEL_FAILOVER_STATE_RECONF_SLAVES:
            return "reconf_slaves";
        case SENTINEL_FAILOVER_STATE_UPDATE_CONFIG:
            return "update_config";
        default:
            return "unknown";
    }
}

/* Redis instance to Redis protocol representation. */
void addReplySentinelRedisInstance(client *c, sentinelRedisInstance *ri) {
    char *flags = sdsempty();
    void *mbl;
    int fields = 0;

    mbl = addDeferredMultiBulkLength(c);

    addReplyBulkCString(c, "name");
    addReplyBulkCString(c, ri->name);
    fields++;

    addReplyBulkCString(c, "ip");
    addReplyBulkCString(c, ri->addr->ip);
    fields++;

    addReplyBulkCString(c, "port");
    addReplyBulkLongLong(c, ri->addr->port);
    fields++;

    addReplyBulkCString(c, "runid");
    addReplyBulkCString(c, ri->runid ? ri->runid : "");
    fields++;

    addReplyBulkCString(c, "flags");
    if (ri->flags & SRI_S_DOWN) flags = sdscat(flags, "s_down,");
    if (ri->flags & SRI_O_DOWN) flags = sdscat(flags, "o_down,");
    if (ri->flags & SRI_MASTER) flags = sdscat(flags, "master,");
    if (ri->flags & SRI_SLAVE) flags = sdscat(flags, "slave,");
    if (ri->flags & SRI_SENTINEL) flags = sdscat(flags, "sentinel,");
    if (ri->link->disconnected) flags = sdscat(flags, "disconnected,");
    if (ri->flags & SRI_MASTER_DOWN) flags = sdscat(flags, "master_down,");
    if (ri->flags & SRI_FAILOVER_IN_PROGRESS)
        flags = sdscat(flags, "failover_in_progress,");
    if (ri->flags & SRI_PROMOTED) flags = sdscat(flags, "promoted,");
    if (ri->flags & SRI_RECONF_SENT) flags = sdscat(flags, "reconf_sent,");
    if (ri->flags & SRI_RECONF_INPROG) flags = sdscat(flags, "reconf_inprog,");
    if (ri->flags & SRI_RECONF_DONE) flags = sdscat(flags, "reconf_done,");

    if (sdslen(flags) != 0) sdsrange(flags, 0, -2); /* remove last "," */
    addReplyBulkCString(c, flags);
    sdsfree(flags);
    fields++;

    addReplyBulkCString(c, "link-pending-commands");
    addReplyBulkLongLong(c, ri->link->pending_commands);
    fields++;

    addReplyBulkCString(c, "link-refcount");
    addReplyBulkLongLong(c, ri->link->refcount);
    fields++;

    if (ri->flags & SRI_FAILOVER_IN_PROGRESS) {
        addReplyBulkCString(c, "failover-state");
        addReplyBulkCString(c, (char *) sentinelFailoverStateStr(ri->failover_state));
        fields++;
    }

    addReplyBulkCString(c, "last-ping-sent");
    addReplyBulkLongLong(c,
                         ri->link->act_ping_time ? (mstime() - ri->link->act_ping_time) : 0);
    fields++;

    addReplyBulkCString(c, "last-ok-ping-reply");
    addReplyBulkLongLong(c, mstime() - ri->link->last_avail_time);
    fields++;

    addReplyBulkCString(c, "last-ping-reply");
    addReplyBulkLongLong(c, mstime() - ri->link->last_pong_time);
    fields++;

    if (ri->flags & SRI_S_DOWN) {
        addReplyBulkCString(c, "s-down-time");
        addReplyBulkLongLong(c, mstime() - ri->s_down_since_time);
        fields++;
    }

    if (ri->flags & SRI_O_DOWN) {
        addReplyBulkCString(c, "o-down-time");
        addReplyBulkLongLong(c, mstime() - ri->o_down_since_time);
        fields++;
    }

    addReplyBulkCString(c, "down-after-milliseconds");
    addReplyBulkLongLong(c, ri->down_after_period);
    fields++;

    /* Masters and Slaves */
    if (ri->flags & (SRI_MASTER | SRI_SLAVE)) {
        addReplyBulkCString(c, "info-refresh");
        addReplyBulkLongLong(c, mstime() - ri->info_refresh);
        fields++;

        addReplyBulkCString(c, "role-reported");
        addReplyBulkCString(c, (ri->role_reported == SRI_MASTER) ? "master" :
                               "slave");
        fields++;

        addReplyBulkCString(c, "role-reported-time");
        addReplyBulkLongLong(c, mstime() - ri->role_reported_time);
        fields++;
    }

    /* Only masters */
    if (ri->flags & SRI_MASTER) {
        addReplyBulkCString(c, "config-epoch");
        addReplyBulkLongLong(c, ri->config_epoch);
        fields++;

        addReplyBulkCString(c, "num-slaves");
        addReplyBulkLongLong(c, dictSize(ri->slaves));
        fields++;

        addReplyBulkCString(c, "num-other-sentinels");
        addReplyBulkLongLong(c, dictSize(ri->sentinels));
        fields++;

        addReplyBulkCString(c, "quorum");
        addReplyBulkLongLong(c, ri->quorum);
        fields++;

        addReplyBulkCString(c, "failover-timeout");
        addReplyBulkLongLong(c, ri->failover_timeout);
        fields++;

        addReplyBulkCString(c, "parallel-syncs");
        addReplyBulkLongLong(c, ri->parallel_syncs);
        fields++;

        if (ri->notification_script) {
            addReplyBulkCString(c, "notification-script");
            addReplyBulkCString(c, ri->notification_script);
            fields++;
        }

        if (ri->client_reconfig_script) {
            addReplyBulkCString(c, "client-reconfig-script");
            addReplyBulkCString(c, ri->client_reconfig_script);
            fields++;
        }
    }

    /* Only slaves */
    if (ri->flags & SRI_SLAVE) {
        addReplyBulkCString(c, "master-link-down-time");
        addReplyBulkLongLong(c, ri->master_link_down_time);
        fields++;

        addReplyBulkCString(c, "master-link-status");
        addReplyBulkCString(c,
                            (ri->slave_master_link_status == SENTINEL_MASTER_LINK_STATUS_UP) ?
                            "ok" : "err");
        fields++;

        addReplyBulkCString(c, "master-host");
        addReplyBulkCString(c,
                            ri->slave_master_host ? ri->slave_master_host : "?");
        fields++;

        addReplyBulkCString(c, "master-port");
        addReplyBulkLongLong(c, ri->slave_master_port);
        fields++;

        addReplyBulkCString(c, "slave-priority");
        addReplyBulkLongLong(c, ri->slave_priority);
        fields++;

        addReplyBulkCString(c, "slave-repl-offset");
        addReplyBulkLongLong(c, ri->slave_repl_offset);
        fields++;
    }

    /* Only sentinels */
    if (ri->flags & SRI_SENTINEL) {
        addReplyBulkCString(c, "last-hello-message");
        addReplyBulkLongLong(c, mstime() - ri->last_hello_time);
        fields++;

        addReplyBulkCString(c, "voted-leader");
        addReplyBulkCString(c, ri->leader ? ri->leader : "?");
        fields++;

        addReplyBulkCString(c, "voted-leader-epoch");
        addReplyBulkLongLong(c, ri->leader_epoch);
        fields++;
    }

    setDeferredMultiBulkLength(c, mbl, fields * 2);
}

/* Output a number of instances contained inside a dictionary as
 * Redis protocol. */
void addReplyDictOfRedisInstances(client *c, dict *instances) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    addReplyMultiBulkLen(c, dictSize(instances));
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        addReplySentinelRedisInstance(c, ri);
    }
    dictReleaseIterator(di);
}

/* Lookup the named master into sentinel.masters.
 * If the master is not found reply to the client with an error and returns
 * NULL. */
sentinelRedisInstance *sentinelGetMasterByNameOrReplyError(client *c,
                                                           robj *name) {
    sentinelRedisInstance *ri;

    ri = dictFetchValue(sentinel.masters, name->ptr);
    if (!ri) {
        addReplyError(c, "No such master with that name");
        return NULL;
    }
    return ri;
}

#define SENTINEL_ISQR_OK 0
#define SENTINEL_ISQR_NOQUORUM (1<<0)
#define SENTINEL_ISQR_NOAUTH (1<<1)

int sentinelIsQuorumReachable(sentinelRedisInstance *master, int *usableptr) {
    dictIterator *di;
    dictEntry *de;
    int usable = 1; /* Number of usable Sentinels. Init to 1 to count myself. */
    int result = SENTINEL_ISQR_OK;
    int voters = dictSize(master->sentinels) + 1; /* Known Sentinels + myself. */

    di = dictGetIterator(master->sentinels);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        if (ri->flags & (SRI_S_DOWN | SRI_O_DOWN)) continue;
        usable++;
    }
    dictReleaseIterator(di);

    if (usable < (int) master->quorum) result |= SENTINEL_ISQR_NOQUORUM;
    if (usable < voters / 2 + 1) result |= SENTINEL_ISQR_NOAUTH;
    if (usableptr) *usableptr = usable;
    return result;
}

void sentinelCommand(client *c) {
    if (!strcasecmp(c->argv[1]->ptr, "masters")) {
        /* SENTINEL MASTERS */
        if (c->argc != 2) goto numargserr;
        addReplyDictOfRedisInstances(c, sentinel.masters);
    } else if (!strcasecmp(c->argv[1]->ptr, "master")) {
        /* SENTINEL MASTER <name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c, c->argv[2]))
            == NULL)
            return;
        addReplySentinelRedisInstance(c, ri);
    } else if (!strcasecmp(c->argv[1]->ptr, "slaves") ||
               !strcasecmp(c->argv[1]->ptr, "replicas")) {
        /* SENTINEL REPLICAS <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c, c->argv[2])) == NULL)
            return;
        addReplyDictOfRedisInstances(c, ri->slaves);
    } else if (!strcasecmp(c->argv[1]->ptr, "sentinels")) {
        /* SENTINEL SENTINELS <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c, c->argv[2])) == NULL)
            return;
        addReplyDictOfRedisInstances(c, ri->sentinels);
    } else if (!strcasecmp(c->argv[1]->ptr, "is-master-down-by-addr")) {
        /* SENTINEL IS-MASTER-DOWN-BY-ADDR <ip> <port> <current-epoch> <runid>
         * 参数：
         * ip:port是要检查的master的ip和port，注意这个命令将不会通过name检查，
         * 因为理论上来说，不同的sentinel可能监控带有相同name的不同master。
         * current-epoch是为了理解我们是否被允许进行一次故障转移的投票。
         * 每一个sentinel对于一个epoch仅能投票一次。
         * runid不为空意味着我们需要为了故障转移而投票，否则将会仅进行查询。*/
        sentinelRedisInstance *ri;
        long long req_epoch;
        uint64_t leader_epoch = 0;
        char *leader = NULL;
        long port;
        int isdown = 0;

        if (c->argc != 6) goto numargserr;
        if (getLongFromObjectOrReply(c, c->argv[3], &port, NULL) != C_OK ||
            getLongLongFromObjectOrReply(c, c->argv[4], &req_epoch, NULL)
            != C_OK)
            return;
        ri = getSentinelRedisInstanceByAddrAndRunID(sentinel.masters,
                                                    c->argv[2]->ptr, port, NULL);

        /* 是否存在，是否是master，是否是主观下线状态？
         * 注意：如果我们处于TILT状态，我们总是回复0 */
        if (!sentinel.tilt && ri && (ri->flags & SRI_S_DOWN) &&
            (ri->flags & SRI_MASTER))
            isdown = 1;

        /* 为这个master投票或者拉取之前的投票结果 */
        if (ri && ri->flags & SRI_MASTER && strcasecmp(c->argv[5]->ptr, "*")) {
            leader = sentinelVoteLeader(ri, (uint64_t) req_epoch,
                                        c->argv[5]->ptr,
                                        &leader_epoch);
        }

        /* 返回回复：down state, leader, vote epoch */
        addReplyMultiBulkLen(c, 3);
        addReply(c, isdown ? shared.cone : shared.czero);
        addReplyBulkCString(c, leader ? leader : "*");
        addReplyLongLong(c, (long long) leader_epoch);
        if (leader) sdsfree(leader);
    } else if (!strcasecmp(c->argv[1]->ptr, "reset")) {
        /* SENTINEL RESET <pattern> */
        if (c->argc != 3) goto numargserr;
        addReplyLongLong(c, sentinelResetMastersByPattern(c->argv[2]->ptr, SENTINEL_GENERATE_EVENT));
    } else if (!strcasecmp(c->argv[1]->ptr, "get-master-addr-by-name")) {
        /* SENTINEL GET-MASTER-ADDR-BY-NAME <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        ri = sentinelGetMasterByName(c->argv[2]->ptr);
        if (ri == NULL) {
            addReply(c, shared.nullmultibulk);
        } else {
            sentinelAddr *addr = sentinelGetCurrentMasterAddress(ri);

            addReplyMultiBulkLen(c, 2);
            addReplyBulkCString(c, addr->ip);
            addReplyBulkLongLong(c, addr->port);
        }
    } else if (!strcasecmp(c->argv[1]->ptr, "failover")) {
        /* SENTINEL FAILOVER <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c, c->argv[2])) == NULL)
            return;
        if (ri->flags & SRI_FAILOVER_IN_PROGRESS) {
            addReplySds(c, sdsnew("-INPROG Failover already in progress\r\n"));
            return;
        }
        if (sentinelSelectSlave(ri) == NULL) {
            addReplySds(c, sdsnew("-NOGOODSLAVE No suitable replica to promote\r\n"));
            return;
        }
        serverLog(LL_WARNING, "Executing user requested FAILOVER of '%s'",
                  ri->name);
        sentinelStartFailover(ri);
        ri->flags |= SRI_FORCE_FAILOVER;
        addReply(c, shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr, "pending-scripts")) {
        /* SENTINEL PENDING-SCRIPTS */

        if (c->argc != 2) goto numargserr;
        sentinelPendingScriptsCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr, "monitor")) {
        /* SENTINEL MONITOR <name> <ip> <port> <quorum> */
        sentinelRedisInstance *ri;
        long quorum, port;
        char ip[NET_IP_STR_LEN];

        if (c->argc != 6) goto numargserr;
        if (getLongFromObjectOrReply(c, c->argv[5], &quorum, "Invalid quorum")
            != C_OK)
            return;
        if (getLongFromObjectOrReply(c, c->argv[4], &port, "Invalid port")
            != C_OK)
            return;

        if (quorum <= 0) {
            addReplyError(c, "Quorum must be 1 or greater.");
            return;
        }

        /* Make sure the IP field is actually a valid IP before passing it
         * to createSentinelRedisInstance(), otherwise we may trigger a
         * DNS lookup at runtime. */
        if (anetResolveIP(NULL, c->argv[3]->ptr, ip, sizeof(ip)) == ANET_ERR) {
            addReplyError(c, "Invalid IP address specified");
            return;
        }

        /* Parameters are valid. Try to create the master instance. */
        ri = createSentinelRedisInstance(c->argv[2]->ptr, SRI_MASTER,
                                         c->argv[3]->ptr, port, quorum, NULL);
        if (ri == NULL) {
            switch (errno) {
                case EBUSY:
                    addReplyError(c, "Duplicated master name");
                    break;
                case EINVAL:
                    addReplyError(c, "Invalid port number");
                    break;
                default:
                    addReplyError(c, "Unspecified error adding the instance");
                    break;
            }
        } else {
            sentinelFlushConfig();
            sentinelEvent(LL_WARNING, "+monitor", ri, "%@ quorum %d", ri->quorum);
            addReply(c, shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr, "flushconfig")) {
        if (c->argc != 2) goto numargserr;
        sentinelFlushConfig();
        addReply(c, shared.ok);
        return;
    } else if (!strcasecmp(c->argv[1]->ptr, "remove")) {
        /* SENTINEL REMOVE <name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c, c->argv[2]))
            == NULL)
            return;
        sentinelEvent(LL_WARNING, "-monitor", ri, "%@");
        dictDelete(sentinel.masters, c->argv[2]->ptr);
        sentinelFlushConfig();
        addReply(c, shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr, "ckquorum")) {
        /* SENTINEL CKQUORUM <name> */
        sentinelRedisInstance *ri;
        int usable;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c, c->argv[2]))
            == NULL)
            return;
        int result = sentinelIsQuorumReachable(ri, &usable);
        if (result == SENTINEL_ISQR_OK) {
            addReplySds(c, sdscatfmt(sdsempty(),
                                     "+OK %i usable Sentinels. Quorum and failover authorization "
                                     "can be reached\r\n", usable));
        } else {
            sds e = sdscatfmt(sdsempty(),
                              "-NOQUORUM %i usable Sentinels. ", usable);
            if (result & SENTINEL_ISQR_NOQUORUM)
                e = sdscat(e, "Not enough available Sentinels to reach the"
                              " specified quorum for this master");
            if (result & SENTINEL_ISQR_NOAUTH) {
                if (result & SENTINEL_ISQR_NOQUORUM) e = sdscat(e, ". ");
                e = sdscat(e, "Not enough available Sentinels to reach the"
                              " majority and authorize a failover");
            }
            e = sdscat(e, "\r\n");
            addReplySds(c, e);
        }
    } else if (!strcasecmp(c->argv[1]->ptr, "set")) {
        if (c->argc < 3) goto numargserr;
        sentinelSetCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr, "info-cache")) {
        /* SENTINEL INFO-CACHE <name> */
        if (c->argc < 2) goto numargserr;
        mstime_t now = mstime();

        /* Create an ad-hoc dictionary type so that we can iterate
         * a dictionary composed of just the master groups the user
         * requested. */
        dictType copy_keeper = instancesDictType;
        copy_keeper.valDestructor = NULL;
        dict *masters_local = sentinel.masters;
        if (c->argc > 2) {
            masters_local = dictCreate(&copy_keeper, NULL);

            for (int i = 2; i < c->argc; i++) {
                sentinelRedisInstance *ri;
                ri = sentinelGetMasterByName(c->argv[i]->ptr);
                if (!ri) continue; /* ignore non-existing names */
                dictAdd(masters_local, ri->name, ri);
            }
        }

        /* Reply format:
         *   1.) master name
         *   2.) 1.) info from master
         *       2.) info from replica
         *       ...
         *   3.) other master name
         *   ...
         */
        addReplyMultiBulkLen(c, dictSize(masters_local) * 2);

        dictIterator *di;
        dictEntry *de;
        di = dictGetIterator(masters_local);
        while ((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
            addReplyBulkCBuffer(c, ri->name, strlen(ri->name));
            addReplyMultiBulkLen(c, dictSize(ri->slaves) + 1); /* +1 for self */
            addReplyMultiBulkLen(c, 2);
            addReplyLongLong(c, now - ri->info_refresh);
            if (ri->info)
                addReplyBulkCBuffer(c, ri->info, sdslen(ri->info));
            else
                addReply(c, shared.nullbulk);

            dictIterator *sdi;
            dictEntry *sde;
            sdi = dictGetIterator(ri->slaves);
            while ((sde = dictNext(sdi)) != NULL) {
                sentinelRedisInstance *sri = dictGetVal(sde);
                addReplyMultiBulkLen(c, 2);
                addReplyLongLong(c, now - sri->info_refresh);
                if (sri->info)
                    addReplyBulkCBuffer(c, sri->info, sdslen(sri->info));
                else
                    addReply(c, shared.nullbulk);
            }
            dictReleaseIterator(sdi);
        }
        dictReleaseIterator(di);
        if (masters_local != sentinel.masters) dictRelease(masters_local);
    } else if (!strcasecmp(c->argv[1]->ptr, "simulate-failure")) {
        /* SENTINEL SIMULATE-FAILURE <flag> <flag> ... <flag> */
        int j;

        sentinel.simfailure_flags = SENTINEL_SIMFAILURE_NONE;
        for (j = 2; j < c->argc; j++) {
            if (!strcasecmp(c->argv[j]->ptr, "crash-after-election")) {
                sentinel.simfailure_flags |=
                        SENTINEL_SIMFAILURE_CRASH_AFTER_ELECTION;
                serverLog(LL_WARNING, "Failure simulation: this Sentinel "
                                      "will crash after being successfully elected as failover "
                                      "leader");
            } else if (!strcasecmp(c->argv[j]->ptr, "crash-after-promotion")) {
                sentinel.simfailure_flags |=
                        SENTINEL_SIMFAILURE_CRASH_AFTER_PROMOTION;
                serverLog(LL_WARNING, "Failure simulation: this Sentinel "
                                      "will crash after promoting the selected replica to master");
            } else if (!strcasecmp(c->argv[j]->ptr, "help")) {
                addReplyMultiBulkLen(c, 2);
                addReplyBulkCString(c, "crash-after-election");
                addReplyBulkCString(c, "crash-after-promotion");
            } else {
                addReplyError(c, "Unknown failure simulation specified");
                return;
            }
        }
        addReply(c, shared.ok);
    } else {
        addReplyErrorFormat(c, "Unknown sentinel subcommand '%s'",
                            (char *) c->argv[1]->ptr);
    }
    return;

    numargserr:
    addReplyErrorFormat(c, "Wrong number of arguments for 'sentinel %s'",
                        (char *) c->argv[1]->ptr);
}

#define info_section_from_redis(section_name) do { \
    if (defsections || allsections || !strcasecmp(section,section_name)) { \
        sds redissection; \
        if (sections++) info = sdscat(info,"\r\n"); \
        redissection = genRedisInfoString(section_name); \
        info = sdscatlen(info,redissection,sdslen(redissection)); \
        sdsfree(redissection); \
    } \
} while(0)

/* SENTINEL INFO [section] */
void sentinelInfoCommand(client *c) {
    if (c->argc > 2) {
        addReply(c, shared.syntaxerr);
        return;
    }

    int defsections = 0, allsections = 0;
    char *section = c->argc == 2 ? c->argv[1]->ptr : NULL;
    if (section) {
        allsections = !strcasecmp(section, "all");
        defsections = !strcasecmp(section, "default");
    } else {
        defsections = 1;
    }

    int sections = 0;
    sds info = sdsempty();

    info_section_from_redis("server");
    info_section_from_redis("clients");
    info_section_from_redis("cpu");
    info_section_from_redis("stats");

    if (defsections || allsections || !strcasecmp(section, "sentinel")) {
        dictIterator *di;
        dictEntry *de;
        int master_id = 0;

        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info,
                            "# Sentinel\r\n"
                            "sentinel_masters:%lu\r\n"
                            "sentinel_tilt:%d\r\n"
                            "sentinel_running_scripts:%d\r\n"
                            "sentinel_scripts_queue_length:%ld\r\n"
                            "sentinel_simulate_failure_flags:%lu\r\n",
                            dictSize(sentinel.masters),
                            sentinel.tilt,
                            sentinel.running_scripts,
                            listLength(sentinel.scripts_queue),
                            sentinel.simfailure_flags);

        di = dictGetIterator(sentinel.masters);
        while ((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
            char *status = "ok";

            if (ri->flags & SRI_O_DOWN) status = "odown";
            else if (ri->flags & SRI_S_DOWN) status = "sdown";
            info = sdscatprintf(info,
                                "master%d:name=%s,status=%s,address=%s:%d,"
                                "slaves=%lu,sentinels=%lu\r\n",
                                master_id++, ri->name, status,
                                ri->addr->ip, ri->addr->port,
                                dictSize(ri->slaves),
                                dictSize(ri->sentinels) + 1);
        }
        dictReleaseIterator(di);
    }

    addReplyBulkSds(c, info);
}

/* Implements Sentinel version of the ROLE command. The output is
 * "sentinel" and the list of currently monitored master names. */
void sentinelRoleCommand(client *c) {
    dictIterator *di;
    dictEntry *de;

    addReplyMultiBulkLen(c, 2);
    addReplyBulkCBuffer(c, "sentinel", 8);
    addReplyMultiBulkLen(c, dictSize(sentinel.masters));

    di = dictGetIterator(sentinel.masters);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        addReplyBulkCString(c, ri->name);
    }
    dictReleaseIterator(di);
}

/* SENTINEL SET <mastername> [<option> <value> ...] */
void sentinelSetCommand(client *c) {
    sentinelRedisInstance *ri;
    int j, changes = 0;
    int badarg = 0; /* Bad argument position for error reporting. */
    char *option;

    if ((ri = sentinelGetMasterByNameOrReplyError(c, c->argv[2]))
        == NULL)
        return;

    /* Process option - value pairs. */
    for (j = 3; j < c->argc; j++) {
        int moreargs = (c->argc - 1) - j;
        option = c->argv[j]->ptr;
        long long ll;
        int old_j = j; /* Used to know what to log as an event. */

        if (!strcasecmp(option, "down-after-milliseconds") && moreargs > 0) {
            /* down-after-millisecodns <milliseconds> */
            robj *o = c->argv[++j];
            if (getLongLongFromObject(o, &ll) == C_ERR || ll <= 0) {
                badarg = j;
                goto badfmt;
            }
            ri->down_after_period = ll;
            sentinelPropagateDownAfterPeriod(ri);
            changes++;
        } else if (!strcasecmp(option, "failover-timeout") && moreargs > 0) {
            /* failover-timeout <milliseconds> */
            robj *o = c->argv[++j];
            if (getLongLongFromObject(o, &ll) == C_ERR || ll <= 0) {
                badarg = j;
                goto badfmt;
            }
            ri->failover_timeout = ll;
            changes++;
        } else if (!strcasecmp(option, "parallel-syncs") && moreargs > 0) {
            /* parallel-syncs <milliseconds> */
            robj *o = c->argv[++j];
            if (getLongLongFromObject(o, &ll) == C_ERR || ll <= 0) {
                badarg = j;
                goto badfmt;
            }
            ri->parallel_syncs = ll;
            changes++;
        } else if (!strcasecmp(option, "notification-script") && moreargs > 0) {
            /* notification-script <path> */
            char *value = c->argv[++j]->ptr;
            if (sentinel.deny_scripts_reconfig) {
                addReplyError(c,
                              "Reconfiguration of scripts path is denied for "
                              "security reasons. Check the deny-scripts-reconfig "
                              "configuration directive in your Sentinel configuration");
                return;
            }

            if (strlen(value) && access(value, X_OK) == -1) {
                addReplyError(c,
                              "Notification script seems non existing or non executable");
                if (changes) sentinelFlushConfig();
                return;
            }
            sdsfree(ri->notification_script);
            ri->notification_script = strlen(value) ? sdsnew(value) : NULL;
            changes++;
        } else if (!strcasecmp(option, "client-reconfig-script") && moreargs > 0) {
            /* client-reconfig-script <path> */
            char *value = c->argv[++j]->ptr;
            if (sentinel.deny_scripts_reconfig) {
                addReplyError(c,
                              "Reconfiguration of scripts path is denied for "
                              "security reasons. Check the deny-scripts-reconfig "
                              "configuration directive in your Sentinel configuration");
                return;
            }

            if (strlen(value) && access(value, X_OK) == -1) {
                addReplyError(c,
                              "Client reconfiguration script seems non existing or "
                              "non executable");
                if (changes) sentinelFlushConfig();
                return;
            }
            sdsfree(ri->client_reconfig_script);
            ri->client_reconfig_script = strlen(value) ? sdsnew(value) : NULL;
            changes++;
        } else if (!strcasecmp(option, "auth-pass") && moreargs > 0) {
            /* auth-pass <password> */
            char *value = c->argv[++j]->ptr;
            sdsfree(ri->auth_pass);
            ri->auth_pass = strlen(value) ? sdsnew(value) : NULL;
            changes++;
        } else if (!strcasecmp(option, "quorum") && moreargs > 0) {
            /* quorum <count> */
            robj *o = c->argv[++j];
            if (getLongLongFromObject(o, &ll) == C_ERR || ll <= 0) {
                badarg = j;
                goto badfmt;
            }
            ri->quorum = ll;
            changes++;
        } else if (!strcasecmp(option, "rename-command") && moreargs > 1) {
            /* rename-command <oldname> <newname> */
            sds oldname = c->argv[++j]->ptr;
            sds newname = c->argv[++j]->ptr;

            if ((sdslen(oldname) == 0) || (sdslen(newname) == 0)) {
                badarg = sdslen(newname) ? j - 1 : j;
                goto badfmt;
            }

            /* Remove any older renaming for this command. */
            dictDelete(ri->renamed_commands, oldname);

            /* If the target name is the same as the source name there
             * is no need to add an entry mapping to itself. */
            if (!dictSdsKeyCaseCompare(NULL, oldname, newname)) {
                oldname = sdsdup(oldname);
                newname = sdsdup(newname);
                dictAdd(ri->renamed_commands, oldname, newname);
            }
            changes++;
        } else {
            addReplyErrorFormat(c, "Unknown option or number of arguments for "
                                   "SENTINEL SET '%s'", option);
            if (changes) sentinelFlushConfig();
            return;
        }

        /* Log the event. */
        int numargs = j - old_j + 1;
        switch (numargs) {
            case 2:
                sentinelEvent(LL_WARNING, "+set", ri, "%@ %s %s", c->argv[old_j]->ptr,
                              c->argv[old_j + 1]->ptr);
                break;
            case 3:
                sentinelEvent(LL_WARNING, "+set", ri, "%@ %s %s %s", c->argv[old_j]->ptr,
                              c->argv[old_j + 1]->ptr,
                              c->argv[old_j + 2]->ptr);
                break;
            default:
                sentinelEvent(LL_WARNING, "+set", ri, "%@ %s", c->argv[old_j]->ptr);
                break;
        }
    }

    if (changes) sentinelFlushConfig();
    addReply(c, shared.ok);
    return;

    badfmt: /* Bad format errors */
    if (changes) sentinelFlushConfig();
    addReplyErrorFormat(c, "Invalid argument '%s' for SENTINEL SET '%s'",
                        (char *) c->argv[badarg]->ptr, option);
}

/* 我们的fake的PUBLISH命令：它用来接收从其它sentinel发来的PUBLISH命令，非SENTINEL_HELLO_CHANNEL通道的消息会返回错误。
 * 因为我们现在有一个Sentinel PUBLISH命令，所有对三种类型的实例代码处理可以是统一的了。 */
void sentinelPublishCommand(client *c) {
    if (strcmp(c->argv[1]->ptr, SENTINEL_HELLO_CHANNEL)) {
        addReplyError(c, "Only HELLO messages are accepted by Sentinel instances.");
        return;
    }
    sentinelProcessHelloMessage(c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
    addReplyLongLong(c, 1);
}

/* ===================== SENTINEL availability checks ======================= */

/* 从我们的视角看这个节点是否是主观下线的 */
void sentinelCheckSubjectivelyDown(sentinelRedisInstance *ri) {
    mstime_t elapsed = 0;

    if (ri->link->act_ping_time)
        elapsed = mstime() - ri->link->act_ping_time;
    else if (ri->link->disconnected)
        elapsed = mstime() - ri->link->last_avail_time;

    /* 检查是否我们需要重连链接
     * 1) 如果命令连接已建立超过15s且发送了PING，但是下线周期已超过一半，还没有收到回复，就重连。
     * 2) 如果订阅连接已建立超过15s且发送了PING，但是已经过3个订阅周期=6s都没有收到回复，就重连。*/
    if (ri->link->cc &&
        (mstime() - ri->link->cc_conn_time) >
        SENTINEL_MIN_LINK_RECONNECT_PERIOD &&
        ri->link->act_ping_time != 0 && /* 有一个PING正在等待响应 */
        /* 这个阻塞的PING延迟了，我们甚至都没有收到错误信息，可能redis在执行一个很长的阻塞命令 */
        (mstime() - ri->link->act_ping_time) > (ri->down_after_period / 2) &&
        (mstime() - ri->link->last_pong_time) > (ri->down_after_period / 2)) {
        instanceLinkCloseConnection(ri->link, ri->link->cc);
    }

    if (ri->link->pc &&
        (mstime() - ri->link->pc_conn_time) >
        SENTINEL_MIN_LINK_RECONNECT_PERIOD &&
        (mstime() - ri->link->pc_last_activity) > (SENTINEL_PUBLISH_PERIOD * 3)) {
        instanceLinkCloseConnection(ri->link, ri->link->pc);
    }

    /* 如果这个实例满足以下两个条件时，我们认为它主观下线了：
     * 1) 在down_after_period时间内，没有收到PING的回复或者没有重连上。
     * 2) 我们认为这是一个master，但是他说自己是一个slave，并且已经报告了很久了。*/
    if (elapsed > ri->down_after_period ||
        (ri->flags & SRI_MASTER &&
         ri->role_reported == SRI_SLAVE &&
         mstime() - ri->role_reported_time >
         (ri->down_after_period + SENTINEL_INFO_PERIOD * 2))) {
        /* 客观下线 */
        if ((ri->flags & SRI_S_DOWN) == 0) {
            sentinelEvent(LL_WARNING, "+sdown", ri, "%@");
            ri->s_down_since_time = mstime();
            ri->flags |= SRI_S_DOWN;
        }
    } else {
        /* 客观上线 */
        if (ri->flags & SRI_S_DOWN) {
            sentinelEvent(LL_WARNING, "-sdown", ri, "%@");
            ri->flags &= ~(SRI_S_DOWN | SRI_SCRIPT_KILL_SENT);
        }
    }
}

/* 根据配置的quorum，该master是否处于客观下线状态。
 * 注意：客观下线是个法定人数，它仅仅意味着在给定的时间内有足够多的sentinels到这个实例是不可达的。
 * 然而这个消息可能会延迟，所有它不是一个强保证，不能保证：
 * N个实例在同一时刻都认为某个实例处于主观下线状态。*/
void sentinelCheckObjectivelyDown(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    unsigned int quorum = 0, odown = 0;

    if (master->flags & SRI_S_DOWN) {
        quorum = 1; /* 当前sentinel认为已经下线 */
        /* 统计其他节点是否认为已经下线 */
        di = dictGetIterator(master->sentinels);
        while ((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);

            if (ri->flags & SRI_MASTER_DOWN) quorum++;
        }
        dictReleaseIterator(di);
        // 如果超过了预设的法定人数，则认为客观下线了
        if (quorum >= master->quorum) odown = 1;
    }

    /* 根据odown设置master状态 */
    if (odown) {
        if ((master->flags & SRI_O_DOWN) == 0) {
            sentinelEvent(LL_WARNING, "+odown", master, "%@ #quorum %d/%d",
                          quorum, master->quorum);
            master->flags |= SRI_O_DOWN;
            master->o_down_since_time = mstime();
        }
    } else {
        if (master->flags & SRI_O_DOWN) {
            sentinelEvent(LL_WARNING, "-odown", master, "%@");
            master->flags &= ~SRI_O_DOWN;
        }
    }
}

/* 接收SENTINEL is-master-down-by-addr命令回复 */
void sentinelReceiveIsMasterDownReply(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = privdata;
    instanceLink *link = c->data;
    redisReply *r;

    if (!reply || !link) return;
    link->pending_commands--;
    r = reply;

    /* 忽略错误或者不期望的回复。
     * 注意：如果命令回复了错误，我们将会在timeout之后清理SRI_MASTER_DOWN标志。
     * 回复格式：0: 主节点下线状态 1：runid 2: epoch */
    if (r->type == REDIS_REPLY_ARRAY && r->elements == 3 &&
        r->element[0]->type == REDIS_REPLY_INTEGER &&
        r->element[1]->type == REDIS_REPLY_STRING &&
        r->element[2]->type == REDIS_REPLY_INTEGER) {
        ri->last_master_down_reply_time = mstime();
        if (r->element[0]->integer == 1) {
            ri->flags |= SRI_MASTER_DOWN;
        } else {
            ri->flags &= ~SRI_MASTER_DOWN;
        }
        if (strcmp(r->element[1]->str, "*")) {
            /* 如果runid不是*，说明对端sentinel进行了一次投票。 */
            sdsfree(ri->leader);
            if ((long long) ri->leader_epoch != r->element[2]->integer)
                serverLog(LL_WARNING,
                          "%s voted for %s %llu", ri->name,
                          r->element[1]->str,
                          (unsigned long long) r->element[2]->integer);
            ri->leader = sdsnew(r->element[1]->str);
            ri->leader_epoch = r->element[2]->integer;
        }
    }
}

/* 如果我们这个master下线了，我们将会发送SENTINEL IS-MASTER-DOWN-BY-ADDR给其他sentinels
 * 获取投票数并且尝试把master标记为客观下线来触发一个故障转移 */
#define SENTINEL_ASK_FORCED (1<<0)

void sentinelAskMasterStateToOtherSentinels(sentinelRedisInstance *master, int flags) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(master->sentinels);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        mstime_t elapsed = mstime() - ri->last_master_down_reply_time;
        char port[32];
        int retval;

        /* 如果从其他sentinel得到state的时间过长，我们认为失效了，就清理掉 */
        if (elapsed > SENTINEL_ASK_PERIOD * 5) {
            ri->flags &= ~SRI_MASTER_DOWN;
            sdsfree(ri->leader);
            ri->leader = NULL;
        }

        /* 仅当满足以下情况时，我们才发送询问消息：
         * 1) 当前master处于主观下线。
         * 2) 和sentinel是连接状态。
         * 3) 在1s内我们没有接受过sentinel is-master-down-by-addr回复信息。*/
        if ((master->flags & SRI_S_DOWN) == 0) continue;
        if (ri->link->disconnected) continue;
        if (!(flags & SENTINEL_ASK_FORCED) &&
            mstime() - ri->last_master_down_reply_time < SENTINEL_ASK_PERIOD)
            continue;

        /* 询问其他sentinel */
        ll2string(port, sizeof(port), master->addr->port);
        retval = redisAsyncCommand(ri->link->cc,
                                   sentinelReceiveIsMasterDownReply, ri,
                                   "%s is-master-down-by-addr %s %s %llu %s",
                                   sentinelInstanceMapCommand(ri, "SENTINEL"),
                                   master->addr->ip, port,
                                   sentinel.current_epoch,
                                   (master->failover_state > SENTINEL_FAILOVER_STATE_NONE) ?
                                   sentinel.myid : "*");
        if (retval == C_OK) ri->link->pending_commands++;
    }
    dictReleaseIterator(di);
}

/* =============================== FAILOVER ================================= */

/* Crash because of user request via SENTINEL simulate-failure command. */
void sentinelSimFailureCrash(void) {
    serverLog(LL_WARNING,
              "Sentinel CRASH because of SENTINEL simulate-failure");
    exit(99);
}

/* Vote for the sentinel with 'req_runid' or return the old vote if already
 * voted for the specified 'req_epoch' or one greater.
 *
 * If a vote is not available returns NULL, otherwise return the Sentinel
 * runid and populate the leader_epoch with the epoch of the vote. */
char *sentinelVoteLeader(sentinelRedisInstance *master, uint64_t req_epoch, char *req_runid, uint64_t *leader_epoch) {
    if (req_epoch > sentinel.current_epoch) {
        sentinel.current_epoch = req_epoch;
        sentinelFlushConfig();
        sentinelEvent(LL_WARNING, "+new-epoch", master, "%llu",
                      (unsigned long long) sentinel.current_epoch);
    }

    if (master->leader_epoch < req_epoch && sentinel.current_epoch <= req_epoch) {
        sdsfree(master->leader);
        master->leader = sdsnew(req_runid);
        master->leader_epoch = sentinel.current_epoch;
        sentinelFlushConfig();
        sentinelEvent(LL_WARNING, "+vote-for-leader", master, "%s %llu",
                      master->leader, (unsigned long long) master->leader_epoch);
        /* If we did not voted for ourselves, set the master failover start
         * time to now, in order to force a delay before we can start a
         * failover for the same master. */
        if (strcasecmp(master->leader, sentinel.myid))
            master->failover_start_time = mstime() + rand() % SENTINEL_MAX_DESYNC;
    }

    *leader_epoch = master->leader_epoch;
    return master->leader ? sdsnew(master->leader) : NULL;
}

struct sentinelLeader {
    char *runid;
    unsigned long votes;
};

/* Helper function for sentinelGetLeader, increment the counter
 * relative to the specified runid. */
int sentinelLeaderIncr(dict *counters, char *runid) {
    dictEntry *existing, *de;
    uint64_t oldval;

    de = dictAddRaw(counters, runid, &existing);
    if (existing) {
        oldval = dictGetUnsignedIntegerVal(existing);
        dictSetUnsignedIntegerVal(existing, oldval + 1);
        return oldval + 1;
    } else {
        serverAssert(de != NULL);
        dictSetUnsignedIntegerVal(de, 1);
        return 1;
    }
}

/* Scan all the Sentinels attached to this master to check if there
 * is a leader for the specified epoch.
 *
 * To be a leader for a given epoch, we should have the majority of
 * the Sentinels we know (ever seen since the last SENTINEL RESET) that
 * reported the same instance as leader for the same epoch. */
char *sentinelGetLeader(sentinelRedisInstance *master, uint64_t epoch) {
    dict *counters;
    dictIterator *di;
    dictEntry *de;
    unsigned int voters = 0, voters_quorum;
    char *myvote;
    char *winner = NULL;
    uint64_t leader_epoch;
    uint64_t max_votes = 0;

    serverAssert(master->flags & (SRI_O_DOWN | SRI_FAILOVER_IN_PROGRESS));
    counters = dictCreate(&leaderVotesDictType, NULL);

    voters = dictSize(master->sentinels) + 1; /* All the other sentinels and me.*/

    /* Count other sentinels votes */
    di = dictGetIterator(master->sentinels);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        if (ri->leader != NULL && ri->leader_epoch == sentinel.current_epoch)
            sentinelLeaderIncr(counters, ri->leader);
    }
    dictReleaseIterator(di);

    /* Check what's the winner. For the winner to win, it needs two conditions:
     * 1) Absolute majority between voters (50% + 1).
     * 2) And anyway at least master->quorum votes. */
    di = dictGetIterator(counters);
    while ((de = dictNext(di)) != NULL) {
        uint64_t votes = dictGetUnsignedIntegerVal(de);

        if (votes > max_votes) {
            max_votes = votes;
            winner = dictGetKey(de);
        }
    }
    dictReleaseIterator(di);

    /* Count this Sentinel vote:
     * if this Sentinel did not voted yet, either vote for the most
     * common voted sentinel, or for itself if no vote exists at all. */
    if (winner)
        myvote = sentinelVoteLeader(master, epoch, winner, &leader_epoch);
    else
        myvote = sentinelVoteLeader(master, epoch, sentinel.myid, &leader_epoch);

    if (myvote && leader_epoch == epoch) {
        uint64_t votes = sentinelLeaderIncr(counters, myvote);

        if (votes > max_votes) {
            max_votes = votes;
            winner = myvote;
        }
    }

    voters_quorum = voters / 2 + 1;
    if (winner && (max_votes < voters_quorum || max_votes < master->quorum))
        winner = NULL;

    winner = winner ? sdsnew(winner) : NULL;
    sdsfree(myvote);
    dictRelease(counters);
    return winner;
}

/* 发送SLAVEOF到指定的实例，我们总是紧跟着一个CONFIG REWRITE的命令来把最新的配置保存到磁盘上
 * 如果host==null，将会发送SLAVEOF NO ONE。
 * 如果SLAVEOF成功入队返回C_OK，否则返回C_ERR。命令的回复将会被丢弃。 */
int sentinelSendSlaveOf(sentinelRedisInstance *ri, char *host, int port) {
    char portstr[32];
    int retval;

    ll2string(portstr, sizeof(portstr), port);

    /* 如果host==null, 我们会发送SLAVEOF NO ONE来转化为一个master */
    if (host == NULL) {
        host = "NO";
        memcpy(portstr, "ONE", 4);
    }

    /* 为了以一种安全的方式发送SLAVEOF，我们将会启动一个事务来完成下面的任务：
     * 1) 根据指定的host/port参数来重新配置这个实例。
     * 2) 重写配置到磁盘上。
     * 3) 关闭所有客户端(不包括发送这个命令的客户端)来保证客户端会触发ask-master-on-reconnection进行重连。
     * 注意：我们不检查命令的返回值，而是通过接下来的INFO命令的回复来观察命令的影响。 */
    retval = redisAsyncCommand(ri->link->cc,
                               sentinelDiscardReplyCallback, ri, "%s",
                               sentinelInstanceMapCommand(ri, "MULTI"));
    if (retval == C_ERR) return retval;
    ri->link->pending_commands++;

    retval = redisAsyncCommand(ri->link->cc,
                               sentinelDiscardReplyCallback, ri, "%s %s %s",
                               sentinelInstanceMapCommand(ri, "SLAVEOF"),
                               host, portstr);
    if (retval == C_ERR) return retval;
    ri->link->pending_commands++;

    retval = redisAsyncCommand(ri->link->cc,
                               sentinelDiscardReplyCallback, ri, "%s REWRITE",
                               sentinelInstanceMapCommand(ri, "CONFIG"));
    if (retval == C_ERR) return retval;
    ri->link->pending_commands++;

    /* CLIENT KILL TYPE <type>仅从Redis 2.8.12开始支持，然后发送CLIENT到一个低版本的实例是没有问题的，
     * 因为CLIENT是个可变的命令，因此Redis将不会认为是符号错误，并且事务也不会失败。 */
    retval = redisAsyncCommand(ri->link->cc,
                               sentinelDiscardReplyCallback, ri, "%s KILL TYPE normal",
                               sentinelInstanceMapCommand(ri, "CLIENT"));
    if (retval == C_ERR) return retval;
    ri->link->pending_commands++;

    retval = redisAsyncCommand(ri->link->cc,
                               sentinelDiscardReplyCallback, ri, "%s",
                               sentinelInstanceMapCommand(ri, "EXEC"));
    if (retval == C_ERR) return retval;
    ri->link->pending_commands++;

    return C_OK;
}

/* 设置master状态来开始一个故障转移 */
void sentinelStartFailover(sentinelRedisInstance *master) {
    serverAssert(master->flags & SRI_MASTER);

    master->failover_state = SENTINEL_FAILOVER_STATE_WAIT_START;
    master->flags |= SRI_FAILOVER_IN_PROGRESS;
    master->failover_epoch = ++sentinel.current_epoch;
    sentinelEvent(LL_WARNING, "+new-epoch", master, "%llu",
                  (unsigned long long) sentinel.current_epoch);
    sentinelEvent(LL_WARNING, "+try-failover", master, "%@");
    master->failover_start_time = mstime() + rand() % SENTINEL_MAX_DESYNC;
    master->failover_state_change_time = mstime();
}

/* 这个实例用来检查是否可以开始故障转移，需要满足以下条件：
 * 1) master必须处于客观下线条件。
 * 2) 没有故障转移正在进行中。
 * 3) 故障转移冷却中：在之前的failover_timeout*2的时间内有一个故障转移开始的企图。
 * 我们还不知道我们是否能够赢得选举，所以有可能我们可是一个故障转移但是不做事情。
 * 如果故障转移开始了，我们将会返回非0。 */
int sentinelStartFailoverIfNeeded(sentinelRedisInstance *master) {
    /* 非客观下线，不开始故障转移 */
    if (!(master->flags & SRI_O_DOWN)) return 0;

    /* 故障转移进行中，不开始 */
    if (master->flags & SRI_FAILOVER_IN_PROGRESS) return 0;

    /* 故障转移，冷却中 */
    if (mstime() - master->failover_start_time <
        master->failover_timeout * 2) {
        if (master->failover_delay_logged != master->failover_start_time) {
            time_t clock = (master->failover_start_time +
                            master->failover_timeout * 2) / 1000;
            char ctimebuf[26];

            ctime_r(&clock, ctimebuf);
            ctimebuf[24] = '\0'; /* Remove newline. */
            master->failover_delay_logged = master->failover_start_time;
            serverLog(LL_WARNING,
                      "Next failover delay: I will not start a failover before %s",
                      ctimebuf);
        }
        return 0;
    }

    sentinelStartFailover(master);
    return 1;
}

/* 选择一个合适的slave来进行晋升。这个算法仅仅允许满足下列的实例：
 * 1) 不带有下列标志：S_DOWN, O_DOWN, DISCONNECTED。
 * 2) 最后收到PING回复的时间不超过5个PING周期。
 * 3) info_refresh不超过3个INFO周期。
 * 4) master_link_down_time到现在的时间不超过：
 *      (now - master->s_down_since_time) + (master->down_after_period * 10)。
 *      基本上，从我们的视角看到master下线，slave将会被断开不超过10个down-after-period。
 *      这个想法是，因为master下线了，所以slave将会堆积，但不应该堆积过久。无论如何，
 *      我们应该根据复制偏移量选择一个最好的slave。
 * 5) slave优先级不能为0，不然我们会放弃这个。
 * 满足以上条件时，我们将会按照以下条件排序：
 * - 更大的优先级
 * - 更大的复制偏移量
 * - 更小字典序的runid
 * 如果找到了合适的slave，将会返回，没找到则返回NULL */
/* sentinelSelectSlave()的辅助函数，被用于qsort()来选出"better first"的slave。 */
int compareSlavesForPromotion(const void *a, const void *b) {
    sentinelRedisInstance **sa = (sentinelRedisInstance **) a,
            **sb = (sentinelRedisInstance **) b;
    char *sa_runid, *sb_runid;

    /* 选择最大优先级 */
    if ((*sa)->slave_priority != (*sb)->slave_priority)
        return (*sa)->slave_priority - (*sb)->slave_priority;

    /* 选择最大复制量 */
    if ((*sa)->slave_repl_offset > (*sb)->slave_repl_offset) {
        return -1; /* a < b */
    } else if ((*sa)->slave_repl_offset < (*sb)->slave_repl_offset) {
        return 1; /* a > b */
    }

    /* 选择最小的runid，注意：低版本的redis不会在INFO发布runid，所以是NULL */
    sa_runid = (*sa)->runid;
    sb_runid = (*sb)->runid;
    if (sa_runid == NULL && sb_runid == NULL) return 0;
    else if (sa_runid == NULL) return 1;  /* a > b */
    else if (sb_runid == NULL) return -1; /* a < b */
    return strcasecmp(sa_runid, sb_runid);
}

sentinelRedisInstance *sentinelSelectSlave(sentinelRedisInstance *master) {
    sentinelRedisInstance **instance =
            zmalloc(sizeof(instance[0]) * dictSize(master->slaves));
    sentinelRedisInstance *selected = NULL;
    int instances = 0;
    dictIterator *di;
    dictEntry *de;
    mstime_t max_master_down_time = 0;

    if (master->flags & SRI_S_DOWN)
        max_master_down_time += mstime() - master->s_down_since_time;
    max_master_down_time += master->down_after_period * 10;

    di = dictGetIterator(master->slaves);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);
        mstime_t info_validity_time;

        if (slave->flags & (SRI_S_DOWN | SRI_O_DOWN)) continue;
        if (slave->link->disconnected) continue;
        if (mstime() - slave->link->last_avail_time > SENTINEL_PING_PERIOD * 5) continue;
        if (slave->slave_priority == 0) continue;

        /* 如果master处于SDOWN状态，我们将会从slaves每1秒获取一次INFO信息，否则是每10s一次 */
        if (master->flags & SRI_S_DOWN)
            info_validity_time = SENTINEL_PING_PERIOD * 5;
        else
            info_validity_time = SENTINEL_INFO_PERIOD * 3;
        if (mstime() - slave->info_refresh > info_validity_time) continue;
        if (slave->master_link_down_time > max_master_down_time) continue;
        instance[instances++] = slave;
    }
    dictReleaseIterator(di);
    if (instances) {
        qsort(instance, instances, sizeof(sentinelRedisInstance *),
              compareSlavesForPromotion);
        selected = instance[0];
    }
    zfree(instance);
    return selected;
}

/* ---------------- Failover state machine implementation ------------------- */
void sentinelFailoverWaitStart(sentinelRedisInstance *ri) {
    char *leader;
    int isleader;

    /* 检查我们是否是当前故障转移纪元的leader */
    leader = sentinelGetLeader(ri, ri->failover_epoch);
    isleader = leader && strcasecmp(leader, sentinel.myid) == 0;
    sdsfree(leader);

    /* 如果我不是领导者，并且也不是通过SENTINEL FAILOVER强行开启的故障转移，那我们不能继续 */
    if (!isleader && !(ri->flags & SRI_FORCE_FAILOVER)) {
        int election_timeout = SENTINEL_ELECTION_TIMEOUT;

        /* 选举超时的时间是max(SENTINEL_ELECTION_TIMEOUT,failover_timeout) */
        if (election_timeout > ri->failover_timeout)
            election_timeout = ri->failover_timeout;
        /* 如果我在选举超时时间内都没有成为leader，则中止故障转移过程 */
        if (mstime() - ri->failover_start_time > election_timeout) {
            sentinelEvent(LL_WARNING, "-failover-abort-not-elected", ri, "%@");
            sentinelAbortFailover(ri);
        }
        return;
    }
    sentinelEvent(LL_WARNING, "+elected-leader", ri, "%@");
    if (sentinel.simfailure_flags & SENTINEL_SIMFAILURE_CRASH_AFTER_ELECTION)
        sentinelSimFailureCrash();
    ri->failover_state = SENTINEL_FAILOVER_STATE_SELECT_SLAVE;
    ri->failover_state_change_time = mstime();
    sentinelEvent(LL_WARNING, "+failover-state-select-slave", ri, "%@");
}

void sentinelFailoverSelectSlave(sentinelRedisInstance *ri) {
    sentinelRedisInstance *slave = sentinelSelectSlave(ri);

    /* 这个状态下我们不处理超时，因为这个函数会自己中止或者进入下一阶段 */
    if (slave == NULL) {
        sentinelEvent(LL_WARNING, "-failover-abort-no-good-slave", ri, "%@");
        sentinelAbortFailover(ri);
    } else {
        sentinelEvent(LL_WARNING, "+selected-slave", slave, "%@");
        slave->flags |= SRI_PROMOTED;
        ri->promoted_slave = slave;
        ri->failover_state = SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE;
        ri->failover_state_change_time = mstime();
        sentinelEvent(LL_NOTICE, "+failover-state-send-slaveof-noone",
                      slave, "%@");
    }
}

void sentinelFailoverSendSlaveOfNoOne(sentinelRedisInstance *ri) {
    int retval;

    /* 如果和要晋升的slave断开了，我们无法发送命令。重试直到超时然后中止 */
    if (ri->promoted_slave->link->disconnected) {
        if (mstime() - ri->failover_state_change_time > ri->failover_timeout) {
            sentinelEvent(LL_WARNING, "-failover-abort-slave-timeout", ri, "%@");
            sentinelAbortFailover(ri);
        }
        return;
    }

    /* 发送SLAVEOF NO ONE到slave，从而把这个slave转换成一个master，
     * 我们注册了一个通用的回调，因为我们不关心回复的内容，
     * 我们将会通过不断检查INFO的返回来判断是否切换成功：slave -> master */
    retval = sentinelSendSlaveOf(ri->promoted_slave, NULL, 0);
    if (retval != C_OK) return;
    sentinelEvent(LL_NOTICE, "+failover-state-wait-promotion",
                  ri->promoted_slave, "%@");
    ri->failover_state = SENTINEL_FAILOVER_STATE_WAIT_PROMOTION;
    ri->failover_state_change_time = mstime();
}

/* 我们将会通过一直检查INFO命令的输出来确定是否这个slave已经转变成了master */
void sentinelFailoverWaitPromotion(sentinelRedisInstance *ri) {
    /* 仅仅处理这个超时。切换到下一个状态是通过解析INFO命令的回复来确定slave的晋升的 */
    if (mstime() - ri->failover_state_change_time > ri->failover_timeout) {
        sentinelEvent(LL_WARNING, "-failover-abort-slave-timeout", ri, "%@");
        sentinelAbortFailover(ri);
    }
}

void sentinelFailoverDetectEnd(sentinelRedisInstance *master) {
    int not_reconfigured = 0, timeout = 0;
    dictIterator *di;
    dictEntry *de;
    mstime_t elapsed = mstime() - master->failover_state_change_time;

    /* 如果这个新晋升的slave不可达，我们不认为故障转移完成 */
    if (master->promoted_slave == NULL ||
        master->promoted_slave->flags & SRI_S_DOWN)
        return;

    /* 如果所有可达的slaves都已经配置好了，则故障转移就结束了 */
    di = dictGetIterator(master->slaves);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);

        if (slave->flags & (SRI_PROMOTED | SRI_RECONF_DONE)) continue;
        if (slave->flags & SRI_S_DOWN) continue;
        not_reconfigured++;
    }
    dictReleaseIterator(di);

    /* 如果故障转移超时了，我们强制结束 */
    if (elapsed > master->failover_timeout) {
        not_reconfigured = 0;
        timeout = 1;
        sentinelEvent(LL_WARNING, "+failover-end-for-timeout", master, "%@");
    }

    if (not_reconfigured == 0) {
        sentinelEvent(LL_WARNING, "+failover-end", master, "%@");
        master->failover_state = SENTINEL_FAILOVER_STATE_UPDATE_CONFIG;
        master->failover_state_change_time = mstime();
    }

    /* 如果是因为超时导致的，则向所有还没有同步master的slaves发送slaveof命令 */
    if (timeout) {
        dictIterator *di;
        dictEntry *de;

        di = dictGetIterator(master->slaves);
        while ((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *slave = dictGetVal(de);
            int retval;

            if (slave->flags & (SRI_RECONF_DONE | SRI_RECONF_SENT)) continue;
            if (slave->link->disconnected) continue;

            retval = sentinelSendSlaveOf(slave,
                                         master->promoted_slave->addr->ip,
                                         master->promoted_slave->addr->port);
            if (retval == C_OK) {
                sentinelEvent(LL_NOTICE, "+slave-reconf-sent-be", slave, "%@");
                slave->flags |= SRI_RECONF_SENT;
            }
        }
        dictReleaseIterator(di);
    }
}

/* 发送slave of <new master address>对所有未完成配置更新的slaves */
void sentinelFailoverReconfNextSlave(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    int in_progress = 0;

    di = dictGetIterator(master->slaves);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);

        if (slave->flags & (SRI_RECONF_SENT | SRI_RECONF_INPROG))
            in_progress++;
    }
    dictReleaseIterator(di);

    di = dictGetIterator(master->slaves);
    while (in_progress < master->parallel_syncs &&
           (de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);
        int retval;

        /* 跳过晋升的slave和已经完成配置的slave */
        if (slave->flags & (SRI_PROMOTED | SRI_RECONF_DONE)) continue;

        /* 如果发送SLAVEOF <new master>之后超过10s，则认为超时，我们认为它已经完成，
         * sentinels将会检查出这种情况并且在之后进行修复 */
        if ((slave->flags & SRI_RECONF_SENT) &&
            (mstime() - slave->slave_reconf_sent_time) >
            SENTINEL_SLAVE_RECONF_TIMEOUT) {
            sentinelEvent(LL_NOTICE, "-slave-reconf-sent-timeout", slave, "%@");
            slave->flags &= ~SRI_RECONF_SENT;
            slave->flags |= SRI_RECONF_DONE;
        }

        /* 对于断连的或者处于同步中的，我们直接跳过 */
        if (slave->flags & (SRI_RECONF_SENT | SRI_RECONF_INPROG)) continue;
        if (slave->link->disconnected) continue;

        /* 发送SLAVEOF <new master>命令 */
        retval = sentinelSendSlaveOf(slave,
                                     master->promoted_slave->addr->ip,
                                     master->promoted_slave->addr->port);
        if (retval == C_OK) {
            slave->flags |= SRI_RECONF_SENT;
            slave->slave_reconf_sent_time = mstime();
            sentinelEvent(LL_NOTICE, "+slave-reconf-sent", slave, "%@");
            in_progress++;
        }
    }
    dictReleaseIterator(di);

    /* 检查是否所有的slaves都已经重新配置或者处理了超时 */
    sentinelFailoverDetectEnd(master);
}


/* 这个函数当slave处于SENTINEL_FAILOVER_STATE_UPDATE_CONFIG状态时被调用。
 * 在这种情况下，我们将会把master从master表中移除，并把晋升的slave加入master表。 */
void sentinelFailoverSwitchToPromotedSlave(sentinelRedisInstance *master) {
    sentinelRedisInstance *ref = master->promoted_slave ?
                                 master->promoted_slave : master;

    sentinelEvent(LL_WARNING, "+switch-master", master, "%s %s %d %s %d",
                  master->name, master->addr->ip, master->addr->port,
                  ref->addr->ip, ref->addr->port);

    sentinelResetMasterAndChangeAddress(master, ref->addr->ip, ref->addr->port);
}

void sentinelFailoverStateMachine(sentinelRedisInstance *ri) {
    serverAssert(ri->flags & SRI_MASTER);

    if (!(ri->flags & SRI_FAILOVER_IN_PROGRESS)) return;

    switch (ri->failover_state) {
        case SENTINEL_FAILOVER_STATE_WAIT_START:
            sentinelFailoverWaitStart(ri);
            break;
        case SENTINEL_FAILOVER_STATE_SELECT_SLAVE:
            sentinelFailoverSelectSlave(ri);
            break;
        case SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE:
            sentinelFailoverSendSlaveOfNoOne(ri);
            break;
        case SENTINEL_FAILOVER_STATE_WAIT_PROMOTION:
            sentinelFailoverWaitPromotion(ri);
            break;
        case SENTINEL_FAILOVER_STATE_RECONF_SLAVES:
            sentinelFailoverReconfNextSlave(ri);
            break;
    }
}

/* 中止一个进行中的故障转移过程。
 * 这个函数仅能在slave晋升到master之前调用，否则将不会中止，并且将一直执行到结束。*/
void sentinelAbortFailover(sentinelRedisInstance *ri) {
    serverAssert(ri->flags & SRI_FAILOVER_IN_PROGRESS);
    serverAssert(ri->failover_state <= SENTINEL_FAILOVER_STATE_WAIT_PROMOTION);

    ri->flags &= ~(SRI_FAILOVER_IN_PROGRESS | SRI_FORCE_FAILOVER);
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = mstime();
    if (ri->promoted_slave) {
        ri->promoted_slave->flags &= ~SRI_PROMOTED;
        ri->promoted_slave = NULL;
    }
}

/* ======================== SENTINEL timer handler ==========================
 * 这是sentinel的主流程，sentinel工作在非阻塞模式下，每秒调用一次
 * -------------------------------------------------------------------------- */

/* 对指定的Redis实例执行调度操作 */
void sentinelHandleRedisInstance(sentinelRedisInstance *ri) {
    /* ========== MONITORING HALF ============ */
    /* 对每种实例都要执行 */
    sentinelReconnectInstance(ri);          // 如果断开，重连
    sentinelSendPeriodicCommands(ri);       // 发送周期命令

    /* ============== ACTING HALF ============= */
    /* 如果我们在TILT模式，则不会执行故障转移的相关操作 */
    if (sentinel.tilt) {
        if (mstime() - sentinel.tilt_start_time < SENTINEL_TILT_PERIOD) return;
        sentinel.tilt = 0;
        sentinelEvent(LL_WARNING, "-tilt", NULL, "#tilt mode exited");
    }

    /* 对master、slave或sentinel检查是否客观下线 */
    sentinelCheckSubjectivelyDown(ri);

    /* 如果是master或者slave */
    if (ri->flags & (SRI_MASTER | SRI_SLAVE)) {
        /* Nothing so far. */
    }

    /* 如果当前实例是master */
    if (ri->flags & SRI_MASTER) {
        sentinelCheckObjectivelyDown(ri);
        if (sentinelStartFailoverIfNeeded(ri))
            sentinelAskMasterStateToOtherSentinels(ri, SENTINEL_ASK_FORCED);
        sentinelFailoverStateMachine(ri);
        sentinelAskMasterStateToOtherSentinels(ri, SENTINEL_NO_FLAGS);
    }
}

/* 对所有监控的master执行调度操作。 */
void sentinelHandleDictOfRedisInstances(dict *instances) {
    dictIterator *di;
    dictEntry *de;
    sentinelRedisInstance *switch_to_promoted = NULL;

    /* 对于每个master，有一些额外的事情要执行 */
    di = dictGetIterator(instances);
    while ((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        sentinelHandleRedisInstance(ri);
        if (ri->flags & SRI_MASTER) {
            // 如果是master，需要递归的处理slaves和sentinels
            sentinelHandleDictOfRedisInstances(ri->slaves);
            sentinelHandleDictOfRedisInstances(ri->sentinels);
            if (ri->failover_state == SENTINEL_FAILOVER_STATE_UPDATE_CONFIG) {
                switch_to_promoted = ri;
            }
        }
    }
    if (switch_to_promoted)
        sentinelFailoverSwitchToPromotedSlave(switch_to_promoted);
    dictReleaseIterator(di);
}

/* 这个函数检查我们是否需要进入TITL模式。
 * 当我们在两次时间中断调用中遇到以下情况时，如果调用时间差为负或者超过了2s，我们会进入TITL模式。
 * 注意：如果我们认为100ms左右是正常的，如果我们需要进入TITL模式，说明我们遇到了以下情况：
 * 1) Sentinel进程因为某些原因阻塞住了，可能是：负载太高，IO冻结，信号停止等等。
 * 2) 系统时钟被修改了。
 * 在上面两种情况下Sentinel会认为出现了超时甚至故障，这是我们进入TILT，并且在SENTINEL_TILT_PERIOD
 * 时间内我们都不执行任何操作。
 * 在TILT期间，我们仍然收集信息，但是我们不执行操作。 */
void sentinelCheckTiltCondition(void) {
    mstime_t now = mstime();
    mstime_t delta = now - sentinel.previous_time;

    if (delta < 0 || delta > SENTINEL_TILT_TRIGGER) {
        sentinel.tilt = 1;
        sentinel.tilt_start_time = mstime();
        sentinelEvent(LL_WARNING, "+tilt", NULL, "#tilt mode entered");
    }
    sentinel.previous_time = mstime();
}

void sentinelTimer(void) {
    sentinelCheckTiltCondition();   // 检查TILT条件
    sentinelHandleDictOfRedisInstances(sentinel.masters);
    sentinelRunPendingScripts();
    sentinelCollectTerminatedScripts();
    sentinelKillTimedoutScripts();

    /* 我们持续的修改Redis定时器中断的频率是为了防止各个sentinel的定时器同步，
     * 从而降低同时发起领导选举的可能性 */
    server.hz = CONFIG_DEFAULT_HZ + rand() % CONFIG_DEFAULT_HZ;
}

