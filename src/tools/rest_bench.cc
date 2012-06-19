// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "include/types.h"
#include "include/atomic.h"

#include "common/obj_bencher.h"
#include "common/config.h"
#include "common/debug.h"
#include "common/ceph_argparse.h"
#include "common/WorkQueue.h"
#include "msg/Message.h"
#include "global/global_init.h"

#include "libs3.h"

#include <deque>

#include <errno.h>

#define DEFAULT_USER_AGENT "rest-bench"
#define DEFAULT_BUCKET "rest-bench-bucket"

void usage(ostream& out)
{
  out <<					\
"usage: rest-bench [options] <write|seq>\n"
"BENCHMARK OPTIONS\n"
"   --seconds\n"
"        benchmak length (default: 60)\n"
"   -t concurrent_operations\n"
"   --concurrent-ios=concurrent_operations\n"
"        select bucket by name\n"
"   -b op-size\n"
"   --block-size=op-size\n"
"        set the size of write ops for put or benchmarking\n"
"   --show-time\n"
"        prefix output lines with date and time\n"
"REST CONFIG OPTIONS\n"
"   --api-host=bhost\n"
"        host name\n"
"   --bucket=bucket\n"
"        select bucket by name\n"
"   --access-key=access_key\n"
"        access key to RESTful storage provider\n"
"   --secret=secret_key\n"
"        secret key for the specified access key\n"
"   --protocol=<http|https>\n"
"        protocol to be used (default: http)\n"
"   --uri_style=<path|vhost>\n"
"        uri style in requests (default: path)\n";
}

static void usage_exit()
{
  usage(cerr);
  exit(1);
}

enum OpType {
  OP_NONE    = 0,
  OP_GET_OBJ = 1,
  OP_PUT_OBJ = 2,
  OP_DELETE_OBJ = 3,
};

struct req_context : public RefCountedObject {
  bool complete;
  S3Status status;
  S3RequestContext *ctx;
  void (*cb)(void *, void *);
  void *arg;
  bufferlist *in_bl;
  bufferlist out_bl;
  uint64_t off;
  uint64_t len;
  string oid;
  Mutex lock;
  Cond cond;
  S3BucketContext *bucket_ctx;

  bool should_destroy_ctx;

  OpType op;

  bool used;

  req_context() : complete(false), status(S3StatusOK), ctx(NULL), cb(NULL), arg(NULL), in_bl(NULL), off(0), len(0),
                  lock("req_context"), bucket_ctx(NULL), should_destroy_ctx(false), op(OP_NONE), used(false) {}
  ~req_context() {
    if (should_destroy_ctx) {
      S3_destroy_request_context(ctx);
    }
  }

  int init_ctx() {
    S3Status status = S3_create_request_context(&ctx);
    if (status != S3StatusOK) {
      cerr << "failed to create context: " << S3_get_status_name(status) << std::endl;
      return -EINVAL;
    }
    should_destroy_ctx = true;

    return 0;
  }

  int ret() {
    if (status != S3StatusOK) {
      return -EINVAL;
    }
    return 0;
  }
};

static S3Status properties_callback(const S3ResponseProperties *properties, void *cb_data)
{
  return S3StatusOK;
}

static void complete_callback(S3Status status, const S3ErrorDetails *details, void *cb_data)
{
  if (!cb_data)
    return;

  struct req_context *ctx = (struct req_context *)cb_data;

  ctx->lock.Lock();
  ctx->status = status;
  ctx->lock.Unlock();

  if (ctx->cb) {
    ctx->cb((void *)ctx->cb, ctx->arg);
  }

  ctx->put();
}

static S3Status get_obj_callback(int size, const char *buf,
                                 void *cb_data)
{
  if (!cb_data)
    return S3StatusOK;

  struct req_context *ctx = (struct req_context *)cb_data;

  ctx->in_bl->append(buf, size);

  return S3StatusOK;
}

static int put_obj_callback(int size, char *buf,
                            void *cb_data)
{
  if (!cb_data)
    return 0;

  struct req_context *ctx = (struct req_context *)cb_data;

  int chunk = ctx->out_bl.length() - ctx->off;
  if (!chunk)
    return 0;

  if (chunk > size)
    chunk = size;

  memcpy(buf, ctx->out_bl.c_str() + ctx->off, chunk);

  ctx->off += chunk;

  return chunk;
}

class RESTDispatcher {
  deque<req_context *> m_req_queue;
  ThreadPool m_tp;

  S3ResponseHandler response_handler;
  S3GetObjectHandler get_obj_handler;
  S3PutObjectHandler put_obj_handler;

  struct DispatcherWQ : public ThreadPool::WorkQueue<req_context> {
    RESTDispatcher *dispatcher;
    DispatcherWQ(RESTDispatcher *p, time_t timeout, time_t suicide_timeout, ThreadPool *tp)
      : ThreadPool::WorkQueue<req_context>("REST", timeout, suicide_timeout, tp), dispatcher(p) {}

    bool _enqueue(req_context *req) {
      dispatcher->m_req_queue.push_back(req);
      _dump_queue();
      return true;
    }
    void _dequeue(req_context *req) {
      assert(0);
    }
    bool _empty() {
      return dispatcher->m_req_queue.empty();
    }
    req_context *_dequeue() {
      if (dispatcher->m_req_queue.empty())
	return NULL;
      req_context *req = dispatcher->m_req_queue.front();
      dispatcher->m_req_queue.pop_front();
      _dump_queue();
      return req;
    }
    void _process(req_context *req) {
      dispatcher->process_context(req);
    }
    void _dump_queue() {
      deque<req_context *>::iterator iter;
      if (dispatcher->m_req_queue.size() == 0) {
        generic_dout(20) << "DispatcherWQ: empty" << dendl;
        return;
      }
      generic_dout(20) << "DispatcherWQ:" << dendl;
      for (iter = dispatcher->m_req_queue.begin(); iter != dispatcher->m_req_queue.end(); ++iter) {
        generic_dout(20) << "req: " << hex << *iter << dec << dendl;
      }
    }
    void _clear() {
      assert(dispatcher->m_req_queue.empty());
    }
  } req_wq;

public:
  RESTDispatcher(CephContext *cct, int num_threads)
    : m_tp(cct, "RESTDispatcher::m_tp", num_threads),
      req_wq(this, g_conf->rgw_op_thread_timeout,
	     g_conf->rgw_op_thread_suicide_timeout, &m_tp) {


    response_handler.propertiesCallback = properties_callback;
    response_handler.completeCallback = complete_callback;

    get_obj_handler.responseHandler = response_handler;
    get_obj_handler.getObjectDataCallback = get_obj_callback;

    put_obj_handler.responseHandler = response_handler;
    put_obj_handler.putObjectDataCallback = put_obj_callback;

  }
  void process_context(req_context *ctx);
  void get_obj(req_context *ctx);
  void put_obj(req_context *ctx);

  void queue(req_context *ctx) {
    req_wq.queue(ctx);
  }

  void start() {
    m_tp.start();
  }
};

void RESTDispatcher::process_context(req_context *ctx)
{
  ctx->get();

  switch (ctx->op) {
    case OP_GET_OBJ:
      get_obj(ctx);
      break;
    case OP_PUT_OBJ:
      put_obj(ctx);
      break;
    default:
      assert(0);
  }

  S3Status status = S3_runall_request_context(ctx->ctx);

  if (status != S3StatusOK) {
    cerr << "ERROR: S3_runall_request_context() returned " << S3_get_status_name(status) << std::endl;
    ctx->status = status;
  } else if (ctx->status != S3StatusOK) {
    cerr << "ERROR: " << ctx->oid << ": " << S3_get_status_name(ctx->status) << std::endl;
  }

  ctx->lock.Lock();
  ctx->complete = true;
  ctx->cond.SignalAll();
  ctx->lock.Unlock();

  ctx->put();
}

void RESTDispatcher::put_obj(req_context *ctx)
{
  S3_put_object(ctx->bucket_ctx, ctx->oid.c_str(),
                ctx->out_bl.length(),
                NULL,
                ctx->ctx,
                &put_obj_handler, ctx);
}

void RESTDispatcher::get_obj(req_context *ctx)
{
  S3_get_object(ctx->bucket_ctx, ctx->oid.c_str(), NULL, 0, ctx->len, ctx->ctx,
                &get_obj_handler, ctx);
}

class RESTBencher : public ObjBencher {
  RESTDispatcher *dispatcher;
  struct req_context **completions;
  struct S3RequestContext **handles;
  S3BucketContext bucket_ctx;
  string user_agent;
  string host;
  string bucket;
  S3Protocol protocol;
  string access_key;
  string secret;
  int concurrentios;

protected:
  int rest_init() {
    S3Status status = S3_initialize(user_agent.c_str(), S3_INIT_ALL, host.c_str());
    if (status != S3StatusOK) {
      cerr << "failed to init: " << S3_get_status_name(status) << std::endl;
      return -EINVAL;
    }


    return 0;
  }


  int completions_init(int _concurrentios) {
    concurrentios = _concurrentios;
    completions = new req_context *[concurrentios];
    handles = new S3RequestContext *[concurrentios];
    for (int i = 0; i < concurrentios; i++) {
      completions[i] = NULL;
      S3Status status = S3_create_request_context(&handles[i]);
      if (status != S3StatusOK) {
        cerr << "failed to create context: " << S3_get_status_name(status) << std::endl;
        return -EINVAL;
      }
    }
    return 0;
  }
  void completions_done() {
    delete[] completions;
    completions = NULL;
    for (int i = 0; i < concurrentios; i++) {
      S3_destroy_request_context(handles[i]);
    }
    delete[] handles;
    handles = NULL;
  }
  int create_completion(int slot, void (*cb)(void *, void*), void *arg) {
    assert (!completions[slot]);

    struct req_context *ctx = new req_context;
    ctx->ctx = handles[slot];
    assert (!ctx->used);
    ctx->used = true;
    ctx->cb = cb;
    ctx->arg = arg;

    completions[slot] = ctx;

    return 0;
  }
  void release_completion(int slot) {
    struct req_context *ctx = completions[slot];

    ctx->used = false;

    ctx->put();
    completions[slot] = 0;
  }

  int aio_read(const std::string& oid, int slot, bufferlist *pbl, size_t len) {
    struct req_context *ctx = completions[slot];

    ctx->get();
    ctx->in_bl = pbl;
    ctx->oid = oid;
    ctx->len = len;
    ctx->bucket_ctx = &bucket_ctx;
    ctx->op = OP_GET_OBJ;

    dispatcher->queue(ctx);

    return 0;
  }

  int aio_write(const std::string& oid, int slot, bufferlist& bl, size_t len) {
    struct req_context *ctx = completions[slot];

    ctx->get();
    ctx->bucket_ctx = &bucket_ctx;
    ctx->out_bl = bl;
    ctx->oid = oid;
    ctx->len = len;
    ctx->op = OP_PUT_OBJ;

    dispatcher->queue(ctx);
    return 0;
  }

  int aio_remove(const std::string& oid, int slot) {
    struct req_context *ctx = completions[slot];

    ctx->get();
    ctx->bucket_ctx = &bucket_ctx;
    ctx->oid = oid;
    ctx->op = OP_DELETE_OBJ;

    dispatcher->queue(ctx);
    return 0;
  }

  int sync_read(const std::string& oid, bufferlist& bl, size_t len) {
    struct req_context *ctx = new req_context;
    int ret = ctx->init_ctx();
    if (ret < 0) {
      return ret;
    }
    ctx->in_bl = &bl;
    ctx->get();
    ctx->bucket_ctx = &bucket_ctx;
    ctx->oid = oid;
    ctx->len = len;
    ctx->op = OP_GET_OBJ;

    dispatcher->process_context(ctx);
    ret = ctx->ret();
    ctx->put();
    return bl.length();
  }
  int sync_write(const std::string& oid, bufferlist& bl, size_t len) {
    struct req_context *ctx = new req_context;
    int ret = ctx->init_ctx();
    if (ret < 0) {
      return ret;
    }
    ctx->get();
    ctx->out_bl = bl;
    ctx->bucket_ctx = &bucket_ctx;
    ctx->oid = oid;
    ctx->op = OP_PUT_OBJ;

    dispatcher->process_context(ctx);
    ret = ctx->ret();
    ctx->put();
    return ret;
  }

  bool completion_is_done(int slot) {
    return completions[slot]->complete;
  }

  int completion_wait(int slot) {
    req_context *ctx = completions[slot];

    Mutex::Locker l(ctx->lock);

    while (!ctx->complete) {
      ctx->cond.Wait(ctx->lock);
    }

    return 0;
  }

  int completion_ret(int slot) {
    S3Status status = completions[slot]->status;
    if (status != S3StatusOK)
      return -EIO;
    return 0;
  }

public:
  RESTBencher(RESTDispatcher *_dispatcher) : dispatcher(_dispatcher), completions(NULL) {
    dispatcher->start();
  }
  ~RESTBencher() { }

  int init(string& _agent, string& _host, string& _bucket, S3Protocol _protocol,
           S3UriStyle uri_style, string& _access_key, string& _secret) {
    user_agent = _agent;
    host = _host;
    bucket = _bucket;
    protocol = _protocol;
    access_key = _access_key;
    secret = _secret;

    bucket_ctx.hostName = NULL; // host.c_str();
    bucket_ctx.bucketName = bucket.c_str();
    bucket_ctx.protocol =  protocol;
    bucket_ctx.accessKeyId = access_key.c_str();
    bucket_ctx.secretAccessKey = secret.c_str();
    bucket_ctx.uriStyle = uri_style;
    
    struct req_context *ctx = new req_context;

    int ret = rest_init();
    if (ret < 0) {
      return ret;
    }

    ret = ctx->init_ctx();
    if (ret < 0) {
      return ret;
    }

    ctx->get();

    S3ResponseHandler response_handler;
    response_handler.propertiesCallback = properties_callback;
    response_handler.completeCallback = complete_callback;

    S3_create_bucket(protocol, access_key.c_str(), secret.c_str(), NULL,
                     bucket.c_str(), S3CannedAclPrivate,
                     NULL, /* locationConstraint */
                     NULL, /* requestContext */
                     &response_handler, /* handler */
                     (void *)ctx  /* callbackData */);

    ret = ctx->ret();
    if (ret < 0) {
      cerr << "ERROR: failed to create bucket: " << S3_get_status_name(ctx->status) << std::endl;
      return ret;
    }

    ctx->put();

    return 0;
  }
};

int main(int argc, const char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  std::map < std::string, std::string > opts;
  std::vector<const char*>::iterator i;
  std::string host;
  std::string val;
  std::string user_agent;
  std::string access_key;
  std::string secret;
  std::string bucket = DEFAULT_BUCKET;
  S3Protocol protocol = S3ProtocolHTTP;
  S3UriStyle uri_style = S3UriStylePath;
  std::string proto_str;
  int concurrent_ios = 16;
  int op_size = 1 << 22;
  int seconds = 60;

  bool show_time = false;


  for (i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_flag(args, i, "-h", "--help", (char*)NULL)) {
      usage(cout);
      exit(0);
    } else if (ceph_argparse_flag(args, i, "--show-time", (char*)NULL)) {
      show_time = true;
    } else if (ceph_argparse_witharg(args, i, &user_agent, "--agent", (char*)NULL)) {
      /* nothing */
    } else if (ceph_argparse_witharg(args, i, &access_key, "--access-key", (char*)NULL)) {
      /* nothing */
    } else if (ceph_argparse_witharg(args, i, &secret, "--secret", (char*)NULL)) {
      /* nothing */
    } else if (ceph_argparse_witharg(args, i, &bucket, "--bucket", (char*)NULL)) {
      /* nothing */
    } else if (ceph_argparse_witharg(args, i, &host, "--api-host", (char*)NULL)) {
      cerr << "host=" << host << std::endl;
      /* nothing */
    } else if (ceph_argparse_witharg(args, i, &proto_str, "--protocol", (char*)NULL)) {
      if (strcasecmp(proto_str.c_str(), "http") == 0) {
        protocol = S3ProtocolHTTP;
      } else if (strcasecmp(proto_str.c_str(), "http") == 0) {
        protocol = S3ProtocolHTTPS;
      } else {
        cerr << "bad protocol" << std::endl;
        usage_exit();
      }
      /* nothing */
    } else if (ceph_argparse_witharg(args, i, &proto_str, "--uri-style", (char*)NULL)) {
      if (strcasecmp(proto_str.c_str(), "vhost") == 0) {
        uri_style = S3UriStyleVirtualHost;
      } else if (strcasecmp(proto_str.c_str(), "path") == 0) {
        uri_style = S3UriStylePath;
      } else {
        cerr << "bad protocol" << std::endl;
        usage_exit();
      }
    } else if (ceph_argparse_witharg(args, i, &val, "-t", "--concurrent-ios", (char*)NULL)) {
      concurrent_ios = strtol(val.c_str(), NULL, 10);
    } else if (ceph_argparse_witharg(args, i, &val, "--seconds", (char*)NULL)) {
      seconds = strtol(val.c_str(), NULL, 10);
    } else if (ceph_argparse_witharg(args, i, &val, "-b", "--block-size", (char*)NULL)) {
      op_size = strtol(val.c_str(), NULL, 10);
    } else {
      if (val[0] == '-')
        usage_exit();
      i++;
    }
  }

  if (bucket.empty()) {
    cerr << "rest-bench: bucket not specified" << std::endl;
    usage_exit();
  }
  if (args.size() < 1)
    usage_exit();
  int operation = 0;
  if (strcmp(args[0], "write") == 0)
    operation = OP_WRITE;
  else if (strcmp(args[0], "seq") == 0)
    operation = OP_SEQ_READ;
  else if (strcmp(args[0], "rand") == 0)
    operation = OP_RAND_READ;
  else
    usage_exit();

  if (host.empty()) {
    cerr << "rest-bench: api host not provided." << std::endl;
    usage_exit();
  }

  if (access_key.empty() || secret.empty()) {
    cerr << "rest-bench: access key or secret was not provided" << std::endl;
    usage_exit();
  }

  if (bucket.empty()) {
    bucket = DEFAULT_BUCKET;
  }

  if (user_agent.empty())
    user_agent = DEFAULT_USER_AGENT;

  RESTDispatcher dispatcher(g_ceph_context, concurrent_ios);

  RESTBencher bencher(&dispatcher);
  bencher.set_show_time(show_time);

  int ret = bencher.init(user_agent, host, bucket, protocol, uri_style, access_key, secret);
  if (ret < 0) {
    cerr << "failed initializing benchmark" << std::endl;
    exit(1);
  }

  ret = bencher.aio_bench(operation, seconds, concurrent_ios, op_size);
  if (ret != 0) {
      cerr << "error during benchmark: " << ret << std::endl;
  }

  return 0;
}

