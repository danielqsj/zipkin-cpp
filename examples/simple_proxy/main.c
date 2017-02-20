#include <unistd.h>
#include <sys/param.h>
#include <sys/socket.h>

#include <string.h>

#include "mongoose.h"

#include "zipkin.h"

#define APP_NAME "simple_proxy"
#define APP_VERSION "1.0"

#define HTTP_VIA "Via"
#define HTTP_X_FORWARDED_FOR "X-Forwarded-For"
#define HTTP_X_FORWARDED_PORT "X-Forwarded-Port"
#define HTTP_X_FORWARDED_PROTO "X-Forwarded-Proto"
#define HTTP_FORWARDED "Forwarded"

static sig_atomic_t s_signal_received = 0;
static const char *s_http_port = "8000";
static struct mg_serve_http_opts s_http_server_opts;

void signal_handler(int sig_num)
{
  signal(sig_num, signal_handler); // Reinstantiate signal handler
  s_signal_received = sig_num;
}

void ev_handler(struct mg_connection *nc, int ev, void *ev_data);

void forward_tcp_connection(struct mg_connection *nc, struct http_message *hm)
{
  const char *errmsg = NULL;
  struct mg_connect_opts opts = {nc, 0, &errmsg};
  char addr[1024] = {0};
  char *p = addr, *end = addr + sizeof(addr) + 1;

  p += snprintf(p, end - p, "tcp://%.*s", (int)hm->uri.len, hm->uri.p);

  nc->user_data = mg_connect_opt(nc->mgr, addr, ev_handler, opts);

  if (nc->user_data)
  {
    mg_send_head(nc, 200, 0, "Proxy-agent: " APP_NAME "/" APP_VERSION);
  }
  else
  {
    mg_http_send_error(nc->user_data, 400, errmsg);
  }
}

void forward_http_request(struct mg_connection *nc, struct http_message *hm)
{
  int i;
  const char *errmsg = NULL;
  struct mg_connect_opts opts = {nc, 0, &errmsg};
  char *uri = strndup(hm->uri.p, hm->uri.len);
  char hostname[MAXHOSTNAMELEN] = {0};
  char local_addr[64] = {0}, peer_addr[64] = {0};
  char extra_headers[4096] = {0};
  char *p = extra_headers, *end = extra_headers + sizeof(extra_headers) - 1;
  struct mg_str *proxy_conn = NULL, *proxy_auth = NULL;

  for (i = 0; i < MG_MAX_HTTP_HEADERS && hm->header_names[i].len > 0; i++)
  {
    struct mg_str *hn = &hm->header_names[i];
    struct mg_str *hv = &hm->header_values[i];

    if (0 == mg_vcmp(hn, "Proxy-Connection"))
    {
      proxy_conn = hv;
    }
    else if (0 != mg_vcmp(hn, "Proxy-Authorization"))
    {
      proxy_auth = hv;
    }
    else
    {
      p += snprintf(p, end - p, "%.*s: %.*s\n", (int)hn->len, hn->p, (int)hv->len, hv->p);
    }
  }

  gethostname(hostname, sizeof(hostname));
  mg_conn_addr_to_str(nc, local_addr, sizeof(local_addr), MG_SOCK_STRINGIFY_IP);
  mg_conn_addr_to_str(nc, peer_addr, sizeof(peer_addr), MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_REMOTE);

  p += snprintf(p, end - p, HTTP_VIA ": 1.1 %s (%s/%s)\n", hostname, APP_NAME, APP_VERSION);
  p += snprintf(p, end - p, HTTP_X_FORWARDED_FOR ": %s\n", peer_addr);
  p += snprintf(p, end - p, HTTP_X_FORWARDED_PORT ": %d\n", nc->sa.sin.sin_port);
  p += snprintf(p, end - p, HTTP_X_FORWARDED_PROTO ": %s\n", "http");
  p += snprintf(p, end - p, HTTP_FORWARDED ": for=%s;proto=http;by=%s\n", peer_addr, local_addr);

  nc->user_data = mg_connect_http_opt(nc->mgr, ev_handler, opts, uri, extra_headers, hm->body.p);

  if (!nc->user_data)
  {
    mg_http_send_error(nc->user_data, 400, errmsg);
  }

  free(uri);
}

void forward_http_response(struct mg_connection *nc, struct http_message *hm)
{
  struct mg_connection *client_conn = (struct mg_connection *)nc->user_data;
  mg_send(client_conn, hm->message.p, hm->message.len);
  client_conn->flags |= MG_F_SEND_AND_CLOSE;
  nc->flags |= MG_F_CLOSE_IMMEDIATELY;
}

void reply_json_response(struct mg_connection *nc, struct http_message *hm)
{
  int i;

  mg_send_response_line(nc, 200,
                        "Content-Type: text/html\r\n"
                        "Connection: close\r\n");
  mg_printf(nc,
            "{\"uri\": \"%.*s\", \"method\": \"%.*s\", \"body\": \"%.*s\", "
            "\"headers\": {",
            (int)hm->uri.len, hm->uri.p, (int)hm->method.len,
            hm->method.p, (int)hm->body.len, hm->body.p);

  for (i = 0; i < MG_MAX_HTTP_HEADERS && hm->header_names[i].len > 0; i++)
  {
    struct mg_str hn = hm->header_names[i];
    struct mg_str hv = hm->header_values[i];
    mg_printf(nc, "%s\"%.*s\": \"%.*s\"", (i != 0 ? "," : ""), (int)hn.len,
              hn.p, (int)hv.len, hv.p);
  }

  mg_printf(nc, "}}");

  nc->flags |= MG_F_SEND_AND_CLOSE;
}

void ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
  struct http_message *hm = (struct http_message *)ev_data;
  struct mg_str *proxy_connection = NULL;

  switch (ev)
  {
  case MG_EV_HTTP_REQUEST:
    if (0 == mg_vcasecmp(&hm->method, "CONNECT"))
    {
      forward_tcp_connection(nc, hm);
    }
    else if (NULL != (proxy_connection = mg_get_http_header(hm, "Proxy-Connection")))
    {
      forward_http_request(nc, hm);
    }
    else
    {
      reply_json_response(nc, hm);
    }

    break;

  case MG_EV_CONNECT:
    if (*(int *)ev_data != 0)
    {
      mg_http_send_error(nc->user_data, 502, NULL);
    }
    break;

  case MG_EV_HTTP_REPLY:
    forward_http_response(nc, hm);
    break;

  case MG_EV_RECV:
    if (nc->user_data)
    {
      mg_send((struct mg_connection *)nc->user_data, nc->recv_mbuf.buf, nc->recv_mbuf.len);
      mbuf_remove(&nc->recv_mbuf, nc->recv_mbuf.len);
    }
    break;

  case MG_EV_CLOSE:
    if (nc->user_data)
    {
      ((struct mg_connection *)nc->user_data)->flags |= MG_F_SEND_AND_CLOSE;
    }
    break;
  }
}

zipkin_tracer_t create_tracer(const char *kafka_uri)
{
  struct mg_str uri = mg_mk_str(kafka_uri), host, path;
  unsigned int port = 0;
  char broker[1024] = {0};
  char *topic;
  zipkin_conf_t conf;
  zipkin_collector_t collector;
  zipkin_tracer_t tracer;

  if (mg_parse_uri(uri, NULL, NULL, &host, &port, &path, NULL, NULL))
    return NULL;

  if (port)
  {
    snprintf(broker, sizeof(broker), "%.*s:%d", (int)host.len, host.p, port);
  }
  else
  {
    snprintf(broker, sizeof(broker), "%.*s", (int)host.len, host.p);
  }

  topic = strtok((char *)path.p, "/");

  conf = zipkin_conf_new(broker, topic);

  if (!conf)
    return NULL;

  collector = zipkin_collector_new(conf);

  zipkin_conf_free(conf);

  if (collector)
  {
    tracer = zipkin_tracer_new(collector, APP_NAME);
  }

  return tracer;
}

int main(int argc, char **argv)
{
  int c;
  const char *kafka_uri = NULL;

  struct mg_mgr mgr;
  struct mg_connection *nc;

  while ((c = getopt(argc, argv, "ht:")) != -1)
  {
    switch (c)
    {
    case 't':
      kafka_uri = optarg;
      break;

    case 'h':
    case '?':
      printf("%s [options]\n\n", argv[0]);
      printf("-t <uri>\tKafka for tracing\n");

      return 1;

    default:
      printf("unknown argument: %c", c);

      abort();
    }
  }

  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);

  mg_mgr_init(&mgr, NULL);

  printf("Starting proxy on port %s\n", s_http_port);
  nc = mg_bind(&mgr, s_http_port, ev_handler);
  if (nc == NULL)
  {
    printf("Failed to create listener\n");
    return 1;
  }

  if (kafka_uri)
  {
    mgr.user_data = create_tracer(kafka_uri);
  }

  mg_set_protocol_http_websocket(nc);

  while (!s_signal_received)
  {
    mg_mgr_poll(&mgr, 1000);
  }
  mg_mgr_free(&mgr);

  return 0;
}