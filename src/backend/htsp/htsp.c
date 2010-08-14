/*
 *  HTSP client
 *  Copyright (C) 2008,2009 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <htsmsg/htsmsg.h>
#include <htsmsg/htsmsg_binary.h>
#include <arch/threads.h>
#include <arch/atomic.h>
#include <libavutil/sha1.h>

#include "showtime.h"
#include "networking/net.h"
#include "navigator.h"
#include "backend/backend.h"
#include "keyring.h"
#include "media.h"
#include "misc/string.h"

#define EPG_TAIL 20          // How many EPG entries to keep per channel

#define HTSP_PROTO_VERSION 1 // Protocol version we implement


static hts_mutex_t htsp_global_mutex;
LIST_HEAD(htsp_connection_list, htsp_connection);
TAILQ_HEAD(htsp_msg_queue, htsp_msg);
LIST_HEAD(htsp_subscription_list, htsp_subscription);
LIST_HEAD(htsp_subscription_stream_list, htsp_subscription_stream);
LIST_HEAD(htsp_tag_list, htsp_tag);
TAILQ_HEAD(htsp_channel_queue, htsp_channel);

static struct htsp_connection_list htsp_connections;

/**
 *
 */
typedef struct htsp_tag {
  LIST_ENTRY(htsp_tag) ht_link;
  char *ht_id;
  char *ht_title;
  unsigned int ht_num_channels;
  int32_t *ht_channels;
  prop_t *ht_root;
} htsp_tag_t;


/**
 *
 */
typedef struct htsp_channel {
  TAILQ_ENTRY(htsp_channel) ch_link;
  int ch_id;
  char *ch_title;
  int ch_channel_num;
  prop_t *ch_root;
} htsp_channel_t;


/**
 *
 */
typedef struct htsp_connection {
  LIST_ENTRY(htsp_connection) hc_global_link;
  tcpcon_t *hc_tc;

  uint8_t hc_challenge[32];

  int hc_is_async;

  int hc_refcount;

  char *hc_hostname;
  int hc_port;

  int hc_seq_generator;
  int hc_sid_generator; /* Subscription ID */

  prop_t *hc_channels_source;
  prop_t *hc_channels_nodes;

  prop_t *hc_tags_source;
  prop_t *hc_tags_nodes;

  hts_mutex_t hc_rpc_mutex;
  hts_cond_t hc_rpc_cond;
  struct htsp_msg_queue hc_rpc_queue;

  hts_mutex_t hc_worker_mutex;
  hts_cond_t hc_worker_cond;
  struct htsp_msg_queue hc_worker_queue;


  hts_mutex_t hc_subscription_mutex;
  struct htsp_subscription_list hc_subscriptions;

  hts_mutex_t hc_meta_mutex;
  struct htsp_tag_list hc_tags;
  struct htsp_channel_queue hc_channels;

} htsp_connection_t;


/**
 *
 */
typedef struct htsp_msg {
  htsmsg_t *hm_msg;
  int hm_error;
  uint32_t hm_seq;
  TAILQ_ENTRY(htsp_msg) hm_link;
} htsp_msg_t;


/**
 *
 */
typedef struct htsp_subscription {
  LIST_ENTRY(htsp_subscription) hs_link;

  uint32_t hs_sid;
  media_pipe_t *hs_mp;
  
  struct htsp_subscription_stream_list hs_streams;

} htsp_subscription_t;


/**
 * Defines a component (audio, video, etc) inside a tv stream
 */
typedef struct htsp_subscription_stream {
  LIST_ENTRY(htsp_subscription_stream) hss_link;

  int hss_index;
  media_codec_t *hss_cw;
  media_queue_t *hss_mq;
  int hss_data_type;

} htsp_subscription_stream_t;




static void htsp_subscriptionStart(htsp_connection_t *hc, htsmsg_t *m);
static void htsp_subscriptionStop(htsp_connection_t *hc, htsmsg_t *m);
static void htsp_subscriptionStatus(htsp_connection_t *hc, htsmsg_t *m);
static void htsp_queueStatus(htsp_connection_t *hc, htsmsg_t *m);
static void htsp_mux_input(htsp_connection_t *hc, htsmsg_t *m);

static htsmsg_t *htsp_reqreply(htsp_connection_t *hc, htsmsg_t *m);



static htsmsg_t *
htsp_recv(htsp_connection_t *hc)
{
  void *buf;
  tcpcon_t *tc = hc->hc_tc;
  uint8_t len[4];
  uint32_t l;

  if(tc->read(tc, len, 4, 1) < 0)
    return NULL;
  
  l = (len[0] << 24) | (len[1] << 16) | (len[2] << 8) | len[3];
  if(l > 16 * 1024 * 1024)
    return NULL;

  buf = malloc(l);

  if(buf == NULL || tc->read(tc, buf, l, 1) < 0) {
    free(buf);
    return NULL;
  }
  
  return htsmsg_binary_deserialize(buf, l, buf); /* consumes 'buf' */
}


/**
 *
 */
static htsmsg_t *
htsp_reqreply(htsp_connection_t *hc, htsmsg_t *m)
{
  void *buf;
  size_t len;
  uint32_t seq;
  int r;
  tcpcon_t *tc = hc->hc_tc;
  uint32_t noaccess;
  htsmsg_t *reply;
  htsp_msg_t *hm = NULL;
  int retry = 0;
  char id[100];
  char *username;
  char *password;
  struct AVSHA1 *shactx = alloca(av_sha1_size);
  uint8_t d[20];

  /* Generate a sequence number for our message */
  seq = atomic_add(&hc->hc_seq_generator, 1);
  htsmsg_add_u32(m, "seq", seq);

 again:

  snprintf(id, sizeof(id), "htsp://%s:%d", hc->hc_hostname, hc->hc_port);

  r = keyring_lookup(id, &username, &password, NULL, !!retry,
		     "TV client", "Access denied");

  if(r == -1) {
    /* User rejected */
    return NULL;
  }

  if(r == 0) {
    /* Got auth credentials */
    htsmsg_delete_field(m, "username");
    htsmsg_delete_field(m, "digest");

    if(username != NULL) 
      htsmsg_add_str(m, "username", username);

    if(password != NULL) {
      av_sha1_init(shactx);
      av_sha1_update(shactx, (const uint8_t *)password, strlen(password));
      av_sha1_update(shactx, hc->hc_challenge, 32);
      av_sha1_final(shactx, d);
      htsmsg_add_bin(m, "digest", d, 20);
    }

    free(username);
    free(password);
  }

  
  


  if(htsmsg_binary_serialize(m, &buf, &len, -1) < 0) {
    htsmsg_destroy(m);
    return NULL;
  }

  if(hc->hc_is_async) {
    /* Async, set up a struct that will be signalled when we get a reply */

    hm = malloc(sizeof(htsp_msg_t));
    hm->hm_msg = NULL;
    hm->hm_seq = seq;
    hm->hm_error = 0;
    hts_mutex_lock(&hc->hc_rpc_mutex);
    TAILQ_INSERT_TAIL(&hc->hc_rpc_queue, hm, hm_link);
    hts_mutex_unlock(&hc->hc_rpc_mutex);
  }

  if(tc->write(tc, buf, len)) {
    free(buf);
    htsmsg_destroy(m);
    
    if(hm != NULL) {
      hts_mutex_lock(&hc->hc_rpc_mutex);
      TAILQ_REMOVE(&hc->hc_rpc_queue, hm, hm_link);
      hts_mutex_unlock(&hc->hc_rpc_mutex);
      free(hm);
    }
    return NULL;
  }
  free(buf);


  if(hm != NULL) {
    hts_mutex_lock(&hc->hc_rpc_mutex);
    while(1) {
      if(hm->hm_error != 0) {
	r = hm->hm_error;
	TAILQ_REMOVE(&hc->hc_rpc_queue, hm, hm_link);
	hts_mutex_unlock(&hc->hc_rpc_mutex);
	free(hm);

	htsmsg_destroy(m);
	return NULL;
      }

      if(hm->hm_msg != NULL)
	break;

      hts_cond_wait(&hc->hc_rpc_cond, &hc->hc_rpc_mutex);
    }
      
    TAILQ_REMOVE(&hc->hc_rpc_queue, hm, hm_link);
    hts_mutex_unlock(&hc->hc_rpc_mutex);
    reply = hm->hm_msg;
    free(hm);

  } else {

    if((reply = htsp_recv(hc)) == NULL) {
      htsmsg_destroy(m);
      return NULL;
    }
  }

  
  if(!htsmsg_get_u32(reply, "noaccess", &noaccess) && noaccess) {
    retry++;
    goto again;
  }

  htsmsg_destroy(m); /* Destroy original message */
  return reply;
}


/**
 *
 */
static int
htsp_login(htsp_connection_t *hc)
{
  const void *ch;
  size_t chlen;
  htsmsg_t *m;

  m = htsmsg_create_map();
  htsmsg_add_str(m, "clientname", "HTS Showtime");
  htsmsg_add_u32(m, "htspversion", 1);
  htsmsg_add_str(m, "method", "hello");
  

  if((m = htsp_reqreply(hc, m)) == NULL) {
    return -1;
  }


  if(htsmsg_get_bin(m, "challenge", &ch, &chlen) || chlen != 32) {
    htsmsg_destroy(m);
    return -1;
  }
  memcpy(hc->hc_challenge, ch, 32);

  htsmsg_destroy(m);


  m = htsmsg_create_map();
  htsmsg_add_str(m, "method", "login");
  htsmsg_add_u32(m, "htspversion", HTSP_PROTO_VERSION);

  if((m = htsp_reqreply(hc, m)) == NULL) {
    return -1;
  }

  htsmsg_destroy(m);

  return 0;
}


/**
 *
 */
static void
update_events(htsp_connection_t *hc, prop_t *metadata, uint32_t id)
{
  int i;
  htsmsg_t *m;
  prop_t *events = prop_create(metadata, "events");
  prop_t *e;
  char buf[10];
  uint32_t u32;

  for(i = 0; i < EPG_TAIL; i++) {
    snprintf(buf, sizeof(buf), "id%d", i);

    if(id != 0) {
      m = htsmsg_create_map();
      htsmsg_add_str(m, "method", "getEvent");
      htsmsg_add_u32(m, "eventId", id);
    
      if((m = htsp_reqreply(hc, m)) != NULL) {

	e = prop_create(events, buf);
	prop_set_string(prop_create(e, "title"), htsmsg_get_str(m, "title"));
	if(!htsmsg_get_u32(m, "start", &u32))
	  prop_set_int(prop_create(e, "start"), u32);
	
	if(!htsmsg_get_u32(m, "stop", &u32))
	  prop_set_int(prop_create(e, "stop"), u32);
	
	id = htsmsg_get_u32_or_default(m, "nextEventId", 0);
	continue;
      } else {
	id = 0;
      }
    }
    prop_destroy_by_name(events, buf);
  }
}


/**
 *
 */
static htsp_channel_t *
htsp_channel_get(htsp_connection_t *hc, int id)
{
  htsp_channel_t *ch;

  TAILQ_FOREACH(ch, &hc->hc_channels, ch_link)
    if(ch->ch_id == id)
      break;
  return ch;
}


/**
 *
 */
static int 
channel_compar(htsp_channel_t *a, htsp_channel_t *b)
{
  if(a->ch_channel_num == b->ch_channel_num)
    return dictcmp(a->ch_title ?: "", b->ch_title ?: "");
  if(a->ch_channel_num < b->ch_channel_num)
    return -1;
  if(a->ch_channel_num > b->ch_channel_num)
    return 1;
  return 0;
}


/**
 *
 */
static void
htsp_channelAddUpdate(htsp_connection_t *hc, htsmsg_t *m, int create)
{
  uint32_t id;
  int chnum;
  prop_t *p, *metadata;
  char txt[200];
  const char *title, *icon;
  htsp_channel_t *ch, *n;

  if(htsmsg_get_u32(m, "channelId", &id))
    return;

  title = htsmsg_get_str(m, "channelName");
  icon  = htsmsg_get_str(m, "channelIcon");
  chnum = htsmsg_get_s32_or_default(m, "channelNumber", -1);

  if(chnum == 0)
    chnum = INT32_MAX;

  snprintf(txt, sizeof(txt), "%d", id);

  hts_mutex_lock(&hc->hc_meta_mutex);

  if(create) {

    ch = calloc(1, sizeof(htsp_channel_t));
    p = ch->ch_root = prop_create(NULL, txt);
    ch->ch_id = id;

    snprintf(txt, sizeof(txt), "htsp://%s:%d/channel/%d",
	     hc->hc_hostname, hc->hc_port, id);
    
    prop_set_string(prop_create(p, "url"), txt);
    prop_set_string(prop_create(p, "type"), "tvchannel");

    ch->ch_channel_num = chnum;
    mystrset(&ch->ch_title, title);

    TAILQ_INSERT_SORTED(&hc->hc_channels, ch, ch_link, channel_compar);
    n = TAILQ_NEXT(ch, ch_link);

    if(prop_set_parent_ex(p, hc->hc_channels_nodes,
			  n ? n->ch_root : NULL, NULL))
      abort();

  } else {

    int move = 0;

    ch = htsp_channel_get(hc, id);
    if(ch == NULL) {
      TRACE(TRACE_ERROR, "HTSP", "Got update for unknown channel %d", id);
      hts_mutex_unlock(&hc->hc_meta_mutex);
      return;
    }

    p = ch->ch_root;

    if(title != NULL) {
      move = 1;
      mystrset(&ch->ch_title, title);
    }

    if(chnum != -1) {
      move = 1;
      ch->ch_channel_num = chnum;
    }

    if(move) {
      TAILQ_REMOVE(&hc->hc_channels, ch, ch_link);
      TAILQ_INSERT_SORTED(&hc->hc_channels, ch, ch_link, channel_compar);
      n = TAILQ_NEXT(ch, ch_link);
      prop_move(p, n ? n->ch_root : NULL);
    }
  }

  hts_mutex_unlock(&hc->hc_meta_mutex);

  metadata = prop_create(p, "metadata");
  if(icon != NULL)
    prop_set_string(prop_create(metadata, "icon"), icon);
  if(title != NULL)
    prop_set_string(prop_create(metadata, "title"), title);
  if(chnum != -1)
    prop_set_int(prop_create(metadata, "channelNumber"), chnum);


  if(htsmsg_get_u32(m, "eventId", &id))
    id = 0;
  update_events(hc, metadata, id);
}


/**
 *
 */
static void
htsp_channelDelete(htsp_connection_t *hc, htsmsg_t *m)
{
  uint32_t id;
  htsp_channel_t *ch;

  if(htsmsg_get_u32(m, "channelId", &id))
    return;

  hts_mutex_lock(&hc->hc_meta_mutex);

  if((ch = htsp_channel_get(hc, id)) != NULL) {
    prop_destroy(ch->ch_root);
    TAILQ_REMOVE(&hc->hc_channels, ch, ch_link);
    free(ch->ch_title);
    free(ch);
  }
  
  hts_mutex_unlock(&hc->hc_meta_mutex);

}


/**
 *
 */
static int 
tag_compar(htsp_tag_t *a, htsp_tag_t *b)
{
  return dictcmp(a->ht_title, b->ht_title);
}


/**
 *
 */
static void
htsp_tagAddUpdate(htsp_connection_t *hc, htsmsg_t *m, int create)
{
  const char *id;
  htsmsg_t *members;
  htsmsg_field_t *f;
  prop_t *metadata, *before, *nodes;
  char txt[200];
  int num = 0, i;
  htsp_tag_t *ht, *n;
  const char *title;
  if((id = htsmsg_get_str(m, "tagId")) == NULL)
    return;
  
  title =  htsmsg_get_str(m, "tagName");

  hts_mutex_lock(&hc->hc_meta_mutex);

  if(create) {

    ht = calloc(1, sizeof(htsp_tag_t));
    ht->ht_id = strdup(id);
    ht->ht_title = strdup(title ?: "");

    LIST_INSERT_SORTED(&hc->hc_tags, ht, ht_link, tag_compar);
    n = LIST_NEXT(ht, ht_link);

    ht->ht_root = prop_create(NULL, id);

    snprintf(txt, sizeof(txt), "htsp://%s:%d/tag/%s",
	     hc->hc_hostname, hc->hc_port, id);
    
    prop_set_string(prop_create(ht->ht_root, "url"), txt);
    prop_set_string(prop_create(ht->ht_root, "type"), "directory");

    if(prop_set_parent_ex(ht->ht_root, hc->hc_tags_nodes,
			  n ? n->ht_root : NULL, NULL))
      abort();
    
  } else {
    
    LIST_FOREACH(ht, &hc->hc_tags, ht_link) {
      if(!strcmp(ht->ht_id, id))
	break;
    }
    
    if(ht == NULL) {
      TRACE(TRACE_ERROR, "HTSP", "Got update for unknown tag %s", id);
      hts_mutex_unlock(&hc->hc_meta_mutex);
      return;
    }

    if(title) {
      mystrset(&ht->ht_title, title);
      LIST_REMOVE(ht, ht_link);

      LIST_INSERT_SORTED(&hc->hc_tags, ht, ht_link, tag_compar);
      n = LIST_NEXT(ht, ht_link);
      prop_move(ht->ht_root, n ? n->ht_root : NULL);

    }
  }

  

  metadata = prop_create(ht->ht_root, "metadata");

  prop_set_string(prop_create(metadata, "title"), htsmsg_get_str(m, "tagName"));
  prop_set_string(prop_create(metadata, "icon"), htsmsg_get_str(m, "tagIcon"));
  prop_set_int(prop_create(metadata, "titledIcon"),
	       htsmsg_get_u32_or_default(m, "tagTitledIcon", 0));

  // Create ordered list of channels in this tag
  nodes = prop_create(ht->ht_root, "nodes");
  if((members = htsmsg_get_list(m, "members")) == NULL) {
    hts_mutex_unlock(&hc->hc_meta_mutex);
    return;
  }

  before = NULL;

  TAILQ_FOREACH_REVERSE(f, &members->hm_fields, htsmsg_field_queue, hmf_link) {
    if(f->hmf_type != HMF_S64)
      continue;
    
    snprintf(txt, sizeof(txt), "%" PRId64, f->hmf_s64);
    prop_t *ch = prop_create(nodes, txt);
    prop_move(ch, before);

    prop_set_string(prop_create(ch, "type"), "tvchannel");
    prop_set_stringf(prop_create(ch, "url"), 
		     "htsp://%s:%d/tagchannel/%s/%" PRId64,
		     hc->hc_hostname, hc->hc_port, id, f->hmf_s64);
    prop_t *orig = prop_create(hc->hc_channels_nodes, txt);

    prop_link(prop_create(orig, "metadata"), prop_create(ch, "metadata"));
    before = ch;
    num++;
  }

  LIST_FOREACH(ht, &hc->hc_tags, ht_link) {
    if(!strcmp(ht->ht_id, id))
      break;
  }

  if(ht == NULL) {
    ht = calloc(1, sizeof(htsp_tag_t));
    LIST_INSERT_HEAD(&hc->hc_tags, ht, ht_link);
    ht->ht_id = strdup(id);
  }

  ht->ht_num_channels = num;
  ht->ht_channels = realloc(ht->ht_channels, num * sizeof(int));

  i = 0;
  TAILQ_FOREACH(f, &members->hm_fields, hmf_link) {
    if(f->hmf_type != HMF_S64)
      continue;
    ht->ht_channels[i++] = f->hmf_s64;
  }

  hts_mutex_unlock(&hc->hc_meta_mutex);
}

/**
 *
 */
static void
htsp_tagDelete(htsp_connection_t *hc, htsmsg_t *m)
{
  htsp_tag_t *ht;
  const char *id;

  if((id = htsmsg_get_str(m, "tagId")) == NULL)
    return;

  hts_mutex_lock(&hc->hc_meta_mutex);

  LIST_FOREACH(ht, &hc->hc_tags, ht_link)
    if(!strcmp(ht->ht_id, id))
      break;

  if(ht != NULL) {
    LIST_REMOVE(ht, ht_link);
    free(ht->ht_id);
    free(ht->ht_title);
    free(ht->ht_channels);
    prop_destroy(ht->ht_root);
    free(ht);
  }
  
  hts_mutex_unlock(&hc->hc_meta_mutex);
}


/**
 *
 * We keep another thread for dispatching unsolicited (asynchronous)
 * messages. Reason is that these messages may in turn cause additional
 * inqueries to the HTSP server and we don't want to block the main input
 * thread with this. Not to mention if the request needs to be retried
 * with new authorization credentials.
 *
 */
static void *
htsp_worker_thread(void *aux)
{
  htsp_connection_t *hc = aux;

  htsp_msg_t *hm;
  htsmsg_t *m;
  const char *method;


  while(1) {
    
    hts_mutex_lock(&hc->hc_worker_mutex);

    while((hm = TAILQ_FIRST(&hc->hc_worker_queue)) == NULL)
      hts_cond_wait(&hc->hc_worker_cond, &hc->hc_worker_mutex);

    TAILQ_REMOVE(&hc->hc_worker_queue, hm, hm_link);
    hts_mutex_unlock(&hc->hc_worker_mutex);

    m = hm->hm_msg;
    free(hm);

    if(m == NULL)
      break;

    if((method = htsmsg_get_str(m, "method")) != NULL) {

      if(!strcmp(method, "channelAdd"))
	htsp_channelAddUpdate(hc, m, 1);
      else if(!strcmp(method, "channelUpdate"))
	htsp_channelAddUpdate(hc, m, 0);
      else if(!strcmp(method, "channelDelete"))
	htsp_channelDelete(hc, m);
      else if(!strcmp(method, "tagAdd"))
	htsp_tagAddUpdate(hc, m, 1);
      else if(!strcmp(method, "tagUpdate"))
	htsp_tagAddUpdate(hc, m, 0);
      else if(!strcmp(method, "tagDelete"))
	htsp_tagDelete(hc, m);
      else if(!strcmp(method, "subscriptionStart"))
	htsp_subscriptionStart(hc, m);
      else if(!strcmp(method, "subscriptionStop"))
	htsp_subscriptionStop(hc, m);
      else if(!strcmp(method, "subscriptionStatus"))
	htsp_subscriptionStatus(hc, m);
      else if(!strcmp(method, "queueStatus"))
	htsp_queueStatus(hc, m);
      else if(!strcmp(method, "initialSyncCompleted")) {
	/* nop for us */
      } else
	TRACE(TRACE_INFO, "HTSP", "Unknown async method '%s' received",
		method);
    }

    htsmsg_destroy(m);
  }
  return NULL;
}




/**
 *
 */
static void
htsp_msg_dispatch(htsp_connection_t *hc, htsmsg_t *m)
{
  uint32_t seq;
  htsp_msg_t *hm;
  const char *method;

  /**
   * Grab streaming input at once
   */
  if((method = htsmsg_get_str(m, "method")) != NULL &&
     !strcmp(method, "muxpkt")) {
    htsp_mux_input(hc, m);
    htsmsg_destroy(m);
    return;
  }

  if(!htsmsg_get_u32(m, "seq", &seq) && seq != 0) {
    /* Reply .. */

    hts_mutex_lock(&hc->hc_rpc_mutex);
    TAILQ_FOREACH(hm, &hc->hc_rpc_queue, hm_link)
      if(seq == hm->hm_seq)
	break;

    if(hm != NULL) {
      hm->hm_msg = m;
      hts_cond_broadcast(&hc->hc_rpc_cond);
      m = NULL;
    } else {
      printf("Warning, unmatched sequence\n");
      abort();
    }
    hts_mutex_unlock(&hc->hc_rpc_mutex);

    if(m != NULL)
      htsmsg_destroy(m);

    return;
  }

  /* Unsolicited meta message */
  /* Async updates are sent to another worker thread */

  hm = malloc(sizeof(htsp_msg_t));

  hm->hm_msg = m;

  hts_mutex_lock(&hc->hc_worker_mutex);
  TAILQ_INSERT_TAIL(&hc->hc_worker_queue, hm, hm_link);
  hts_cond_signal(&hc->hc_worker_cond);
  hts_mutex_unlock(&hc->hc_worker_mutex);
}


/**
 *
 */
static void *
htsp_thread(void *aux)
{
  htsp_connection_t *hc = aux;
  htsmsg_t *m;

  while(1) {

    m = htsmsg_create_map();

    htsmsg_add_str(m, "method", "enableAsyncMetadata");
    m = htsp_reqreply(hc, m);
    if(m == NULL) {
      return NULL;
    }
    htsmsg_destroy(m);

    hc->hc_is_async = 1;

    while(1) {

      if((m = htsp_recv(hc)) == NULL) {
	TRACE(TRACE_ERROR, "HTSP", "Read failed");
	break;
      }
      htsp_msg_dispatch(hc, m);
    }
    abort(); // We cant handle this now
  }
  return NULL;
}



/**
 *
 */
static htsp_connection_t *
htsp_connection_find(const char *url, char *path, size_t pathlen,
		     char *errbuf, size_t errlen)
{
  htsp_connection_t *hc;
  int port;
  char hostname[HOSTNAME_MAX];
  prop_t *meta, *nodes;
  tcpcon_t *tc;

  url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), &port,
	    path, pathlen, url, 0);

  if(port < 0)
    port = 9982;

  hts_mutex_lock(&htsp_global_mutex);

  LIST_FOREACH(hc, &htsp_connections, hc_global_link) {
    if(!strcmp(hc->hc_hostname, hostname) && hc->hc_port == port) {
      hc->hc_refcount++;
      hts_mutex_unlock(&htsp_global_mutex);
      return hc;
    }
  }

  trace(TRACE_DEBUG, "HTSP", "Connecting to %s:%d", hostname, port);

  tc = tcp_connect(hostname, port, errbuf, errlen, 3000, 0);
  if(tc == NULL) {
    hts_mutex_unlock(&htsp_global_mutex);
    trace(TRACE_ERROR, "HTSP", "Connection to %s:%d failed: %s", 
	  hostname, port, errbuf);
    return NULL;
  }

  trace(TRACE_INFO, "HTSP", "Connected to %s:%d", hostname, port);

  hc = calloc(1, sizeof(htsp_connection_t));

  hc->hc_tags_source = prop_create(NULL, NULL);
  hc->hc_tags_nodes  = prop_create(hc->hc_tags_source, "nodes");
  prop_set_string(prop_create(hc->hc_tags_source, "type"), "directory");
  meta = prop_create(hc->hc_tags_source, "metadata");
  prop_set_string(prop_create(meta, "title"), "Channel groups");

  
  nodes = prop_create(hc->hc_tags_source, "nodes");


  hc->hc_channels_source = prop_create(nodes, NULL);
  hc->hc_channels_nodes  = prop_create(hc->hc_channels_source, "nodes");
  prop_set_string(prop_create(hc->hc_channels_source, "type"), "directory");
  meta = prop_create(hc->hc_channels_source, "metadata");
  prop_set_string(prop_create(meta, "title"), "All channels");
  prop_set_stringf(prop_create(hc->hc_channels_source, "url"),
		   "htsp://%s:%d/channels", hostname, port);
  prop_set_string(prop_create(hc->hc_channels_source, "type"),
		  "directory");

    
  hts_mutex_init(&hc->hc_rpc_mutex);
  hts_cond_init(&hc->hc_rpc_cond);
  TAILQ_INIT(&hc->hc_rpc_queue);

  hts_mutex_init(&hc->hc_worker_mutex);
  hts_cond_init(&hc->hc_worker_cond);
  TAILQ_INIT(&hc->hc_worker_queue);

  hts_mutex_init(&hc->hc_subscription_mutex);

  hts_mutex_init(&hc->hc_meta_mutex);
  TAILQ_INIT(&hc->hc_channels);

  hc->hc_tc = tc;
  hc->hc_seq_generator = 1;
  hc->hc_sid_generator = 1;
  hc->hc_hostname = strdup(hostname);
  hc->hc_port = port;

  hc->hc_refcount = 1;

  LIST_INSERT_HEAD(&htsp_connections, hc, hc_global_link);

  htsp_login(hc);

  hts_thread_create_detached("HTSP main", htsp_thread, hc);
  hts_thread_create_detached("HTSP worker", htsp_worker_thread, hc);

  hts_mutex_unlock(&htsp_global_mutex);
  return hc;
}

/**
 *
 */
static void
htsp_connection_deref(htsp_connection_t *hc)
{
  abort();
}


/**
 *
 */
static nav_page_t *
be_htsp_open(backend_t *be, struct navigator *nav,
	     const char *url, const char *view,
	     char *errbuf, size_t errlen)
{
  htsp_connection_t *hc;
  nav_page_t *np;
  prop_t *p, *src;
  char path[URL_MAX];

  if((hc = htsp_connection_find(url, path, sizeof(path), 
				errbuf, errlen)) == NULL) {
    return NULL;
  }

  TRACE(TRACE_DEBUG, "HTSP", "Open %s", url);

  if(!strncmp(path, "/channel/", strlen("/channel/")) ||
     !strncmp(path, "/tagchannel/", strlen("/tagchannel/")))
    return backend_open_video(be, nav, url, view, errbuf, errlen);

  if(!strcmp(path, "/channels")) {
    src = hc->hc_channels_source;
  } else if(!strncmp(path, "/tag/", strlen("/tag/"))) {
    src = prop_create(hc->hc_tags_nodes, path + strlen("/tag/"));
  } else if(!strcmp(path, "")) {
    src = hc->hc_tags_source;
  } else {
    snprintf(errbuf, errlen, "Invalid URL");
    htsp_connection_deref(hc);
    return NULL;
  }

  np = nav_page_create(nav, url, view, NAV_PAGE_DONT_CLOSE_ON_BACK);

  p = np->np_prop_root;
  prop_set_string(prop_create(p, "view"), "list");
  prop_link(src, prop_create(p, "model"));

  return np;
}


/**
 *
 */
static void
htsp_set_subtitles(media_pipe_t *mp, const char *id)
{
  if(!strcmp(id, "off")) {
    mp->mp_video.mq_stream2 = -1;
    prop_set_string(mp->mp_prop_subtitle_track_current, "sub:off");
    
  } else {
    unsigned int idx = atoi(id);

    mp->mp_video.mq_stream2 = idx;
    prop_set_stringf(mp->mp_prop_subtitle_track_current, "sub:%d", idx);
  }
}


/**
 *
 */
static void
htsp_set_audio(media_pipe_t *mp, const char *id)
{
  if(!strcmp(id, "off")) {
    mp->mp_audio.mq_stream = -1;
    prop_set_string(mp->mp_prop_audio_track_current, "audio:off");
    
  } else {
    unsigned int idx = atoi(id);

    mp->mp_audio.mq_stream = idx;
    prop_set_stringf(mp->mp_prop_audio_track_current, "audio:%d", idx);
  }
}


/**
 *
 */
static void
set_channel(htsp_connection_t *hc, htsp_subscription_t *hs, int chid,
	    char **name)
{
  htsp_channel_t *ch;
  hts_mutex_lock(&hc->hc_meta_mutex);

  if((ch = htsp_channel_get(hc, chid)) != NULL) {
    TRACE(TRACE_DEBUG, "HTSP", "Subscribing to channel %s", ch->ch_title);
    prop_set_string(prop_create(hs->hs_mp->mp_prop_metadata, "title"),
		    ch->ch_title);
    mystrset(name, ch->ch_title);
  } else {
    mystrset(name, NULL);
  }
  
  hts_mutex_unlock(&hc->hc_meta_mutex);
}

/**
 *
 */
static int
zap_channel(htsp_connection_t *hc, htsp_subscription_t *hs,
	    int chid, char *errbuf, size_t errlen, const char *tag, int delta,
	    char **name)
{
  htsp_tag_t *ht;
  htsp_channel_t *ch;
  int p, newch;
  htsmsg_t *m;
  const char *err;

  hts_mutex_lock(&hc->hc_meta_mutex);

  if(tag != NULL) {

    LIST_FOREACH(ht, &hc->hc_tags, ht_link)
      if(!strcmp(ht->ht_id, tag))
	break;
  
    if(ht == NULL) {
      hts_mutex_unlock(&hc->hc_meta_mutex);
      return chid;
    }

    for(p = 0; p < ht->ht_num_channels; p++)
      if(ht->ht_channels[p] == chid)
	break;

    if(p == ht->ht_num_channels) {
      hts_mutex_unlock(&hc->hc_meta_mutex);
      return chid;
    }

    p += delta;
    if(p == -1)
      p = ht->ht_num_channels - 1;
    else if(p == ht->ht_num_channels)
      p = 0;

    newch = ht->ht_channels[p];

  } else {

    if((ch = htsp_channel_get(hc, chid)) == NULL) {
      hts_mutex_unlock(&hc->hc_meta_mutex);
      return chid;
    }
    
    if(delta == 1) {
      if((ch = TAILQ_NEXT(ch, ch_link)) == NULL)
	ch = TAILQ_FIRST(&hc->hc_channels);
    } else {
      if((ch = TAILQ_PREV(ch, htsp_channel_queue, ch_link)) == NULL)
	ch = TAILQ_LAST(&hc->hc_channels, htsp_channel_queue);
    }

    if(ch == NULL) {
      hts_mutex_unlock(&hc->hc_meta_mutex);
      return chid;
    }

    newch = ch->ch_id;
  }



  hts_mutex_unlock(&hc->hc_meta_mutex);

  // Stop current

  m = htsmsg_create_map();

  htsmsg_add_str(m, "method", "unsubscribe");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);

  if((m = htsp_reqreply(hc, m)) == NULL) {
    snprintf(errbuf, errlen, "Connection with server lost");
    return -1;
  }
  htsmsg_destroy(m);

  hts_mutex_lock(&hc->hc_subscription_mutex);
  hs->hs_sid = atomic_add(&hc->hc_sid_generator, 1);

  mp_flush(hs->hs_mp, 1);
  hts_mutex_unlock(&hc->hc_subscription_mutex);

  m = htsmsg_create_map();

  htsmsg_add_str(m, "method", "subscribe");
  htsmsg_add_u32(m, "channelId", newch);
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);

  if((m = htsp_reqreply(hc, m)) == NULL) {
    snprintf(errbuf, errlen, "Connection with server lost");
    return -1;
  }

  if((err = htsmsg_get_str(m, "error")) != NULL) {
    snprintf(errbuf, errlen, "From server: %s", err);
    htsmsg_destroy(m);
    return -1;
  }

  htsmsg_destroy(m);
  set_channel(hc, hs, newch, name);
  return newch;
}


/**
 *
 */
static int
prio_to_weight(int p)
{
  int w;

  if(p == 0)
    return 150;

  w = 140 - p;
  if(w < 110)
    w = 110;
  return w;
}


/**
 *
 */
static event_t *
htsp_subscriber(htsp_connection_t *hc, htsp_subscription_t *hs, 
		int chid, char *errbuf, size_t errlen, const char *tag,
		int primary, int priority)
{
  event_t *e;
  htsmsg_t *m;
  media_pipe_t *mp = hs->hs_mp;
  const char *err;
  char *name = NULL;

  m = htsmsg_create_map();

  htsmsg_add_str(m, "method", "subscribe");
  htsmsg_add_u32(m, "channelId", chid);
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);
  htsmsg_add_u32(m, "weight", prio_to_weight(priority));

  if((m = htsp_reqreply(hc, m)) == NULL) {
    snprintf(errbuf, errlen, "Connection with server lost");
    return NULL;
  }

  if((err = htsmsg_get_str(m, "error")) != NULL) {
    snprintf(errbuf, errlen, "From server: %s", err);
    htsmsg_destroy(m);
    return NULL;
  }

  htsmsg_destroy(m);

  prop_set_string(mp->mp_prop_playstatus, "play");

  if(primary)
    mp_become_primary(mp);
  else
    mp_init_audio(mp);

  set_channel(hc, hs, chid, &name);

  while(1) {
    e = mp_dequeue_event(mp);
    
    if(event_is_type(e, EVENT_SELECT_TRACK)) {
      event_select_track_t *est = (event_select_track_t *)e;

      if(!strncmp(est->id, "sub:", strlen("sub:")))
	htsp_set_subtitles(mp, est->id + strlen("sub:"));
      else if(!strncmp(est->id, "audio:", strlen("audio:")))
	htsp_set_audio(mp, est->id + strlen("audio:"));

    } else if(event_is_type(e, EVENT_PLAYBACK_PRIORITY)) {
      event_int_t *ei = (event_int_t *)e;

      TRACE(TRACE_DEBUG, "HTSP", "%s: Changed priority to %d",
	    name, ei->val);

      m = htsmsg_create_map();
      
      htsmsg_add_str(m, "method", "subscriptionChangeWeight");
      htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);
      htsmsg_add_u32(m, "weight", prio_to_weight(ei->val));

      if((m = htsp_reqreply(hc, m)) == NULL) {
	snprintf(errbuf, errlen, "Connection with server lost");
	return NULL;
      }
      
      if((err = htsmsg_get_str(m, "error")) != NULL) {
	snprintf(errbuf, errlen, "From server: %s", err);
	htsmsg_destroy(m);
	return NULL;
      }
      
      htsmsg_destroy(m);

    } else if(event_is_action(e, ACTION_PREV_CHANNEL) ||
	      event_is_action(e, ACTION_PREV_TRACK)) {

      chid = zap_channel(hc, hs, chid, errbuf, errlen, tag, -1, &name);

    } else if(event_is_action(e, ACTION_NEXT_CHANNEL) ||
	      event_is_action(e, ACTION_NEXT_TRACK)) {

      chid = zap_channel(hc, hs, chid, errbuf, errlen, tag, 1, &name);

    } else if(event_is_type(e, EVENT_EXIT) ||
	      event_is_type(e, EVENT_PLAY_URL))
      break;

    event_unref(e);

    if(chid == -1)
      return NULL;

  }

  prop_set_string(mp->mp_prop_playstatus, "stop");
  
  m = htsmsg_create_map();

  htsmsg_add_str(m, "method", "unsubscribe");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);

  if((m = htsp_reqreply(hc, m)) != NULL)
    htsmsg_destroy(m);

  return e;
}


/**
 *
 */
static void
htsp_free_streams(htsp_subscription_t *hs)
{
  htsp_subscription_stream_t *hss;
  
  while((hss = LIST_FIRST(&hs->hs_streams)) != NULL) {
    LIST_REMOVE(hss, hss_link);
    media_codec_deref(hss->hss_cw);
    free(hss);
  }
}


/**
 *
 */
static event_t *
be_htsp_playvideo(backend_t *be, 
		  const char *url, media_pipe_t *mp, 
		  int primary, int priority,
		  char *errbuf, size_t errlen)
{
  htsp_connection_t *hc;
  char path[URL_MAX];
  htsp_subscription_t *hs;
  event_t *e;
  char *tag = NULL;
  int chid;


  TRACE(TRACE_DEBUG, "HTSP",
	"Starting video playback %s, primary=%s, priority=%d",
	url, primary ? "yes" : "no", priority);

  if((hc = htsp_connection_find(url, path, sizeof(path), 
				errbuf, errlen)) == NULL) {
    return NULL;
  }

  if(!strncmp(path, "/channel/", strlen("/channel/"))) {
    chid = atoi(path + strlen("/channel/"));
    tag = NULL;
  } else if(!strncmp(path, "/tagchannel/", strlen("/tagchannel/"))) {
    tag = mystrdupa(path + strlen("/tagchannel/"));
    char *x = strrchr(tag, '/');
    if(x == NULL) {
      snprintf(errbuf, errlen, "Invalid URL");
      htsp_connection_deref(hc);
      return NULL;
    }
    *x++ = 0;
    chid = atoi(x);
  } else {
    snprintf(errbuf, errlen, "Invalid URL");
    htsp_connection_deref(hc);
    return NULL;
  }


  hs = calloc(1, sizeof(htsp_subscription_t));

  hs->hs_sid = atomic_add(&hc->hc_sid_generator, 1);
  hs->hs_mp = mp;

  hts_mutex_lock(&hc->hc_subscription_mutex);
  LIST_INSERT_HEAD(&hc->hc_subscriptions, hs, hs_link);
  hts_mutex_unlock(&hc->hc_subscription_mutex);

  e = htsp_subscriber(hc, hs, chid, errbuf, errlen, tag, primary, priority);
  mp_flush(mp, 0);
  mp_flush(mp, 0);
  mp_shutdown(mp);

  hts_mutex_lock(&hc->hc_subscription_mutex);
  LIST_REMOVE(hs, hs_link);
  hts_mutex_unlock(&hc->hc_subscription_mutex);

  htsp_free_streams(hs);
  free(hs);
  return e;
}

/**
 * Leaves 'hc_subscription_mutex' locked if we successfully find a subscription
 */
static htsp_subscription_t *
htsp_find_subscription_by_msg(htsp_connection_t *hc, htsmsg_t *m)
{
  uint32_t sid;
  htsp_subscription_t *hs;

  if(htsmsg_get_u32(m, "subscriptionId", &sid))
    return NULL;

  hts_mutex_lock(&hc->hc_subscription_mutex);
  LIST_FOREACH(hs, &hc->hc_subscriptions, hs_link)
    if(hs->hs_sid == sid)
      return hs;

  hts_mutex_unlock(&hc->hc_subscription_mutex);
  return NULL;
}


/**
 * Transport input
 */
static void
htsp_mux_input(htsp_connection_t *hc, htsmsg_t *m)
{
  htsp_subscription_t *hs;
  htsp_subscription_stream_t *hss;
  uint32_t stream;
  media_pipe_t *mp;
  const void *bin;
  size_t binlen;
  media_buf_t *mb;

  if(htsmsg_get_u32(m, "stream", &stream)  ||
     htsmsg_get_bin(m, "payload", &bin, &binlen))
    return;
  
  if((hs = htsp_find_subscription_by_msg(hc, m)) == NULL)
    return;

  mp = hs->hs_mp;

  if(stream == mp->mp_audio.mq_stream || stream == mp->mp_video.mq_stream ||
     stream == mp->mp_video.mq_stream2) {

    LIST_FOREACH(hss, &hs->hs_streams, hss_link)
      if(hss->hss_index == stream)
	break;
      
    if(hss != NULL) {

      mb = media_buf_alloc();
      mb->mb_data_type = hss->hss_data_type;
      mb->mb_stream = hss->hss_index;

      if(htsmsg_get_u32(m, "duration", &mb->mb_duration))
	mb->mb_duration = 0;

      if(htsmsg_get_s64(m, "dts", &mb->mb_dts))
	mb->mb_dts = AV_NOPTS_VALUE;

      if(htsmsg_get_s64(m, "pts", &mb->mb_pts))
	mb->mb_pts = AV_NOPTS_VALUE;

      mb->mb_epoch = 1;

      mb->mb_cw = media_codec_ref(hss->hss_cw);

      mb->mb_data = malloc(binlen + FF_INPUT_BUFFER_PADDING_SIZE);
      memcpy(mb->mb_data, bin, binlen);
      memset(mb->mb_data + binlen, 0, FF_INPUT_BUFFER_PADDING_SIZE);
  
      mb->mb_size = binlen;

      if(mb_enqueue_no_block(mp, hss->hss_mq, mb))
	media_buf_free(mb);
    }
  }
  hts_mutex_unlock(&hc->hc_subscription_mutex);
}


/**
 *
 */
static void
htsp_subscriptionStart(htsp_connection_t *hc, htsmsg_t *m)
{
  media_pipe_t *mp;
  htsp_subscription_t *hs;
  htsmsg_field_t *f;
  htsmsg_t *sub, *streams;
  const char *type;
  uint32_t idx, s;
  enum CodecID   codec_id;
  enum CodecType codec_type;
  const char *nicename, *title;
  media_codec_t *cw;

  int vstream = -1; /* Initial video stream */
  int astream = -1; /* Initial audio stream */
  int sstream = -1; /* Initial subtitle stream */

  /* Discriminators for chosing best stream */
  int ascore = 0;
  int vscore = 0;
  int sscore = 0;

  int subid;
  prop_t *metaparent = NULL;
  htsp_subscription_stream_t *hss;
  char titlebuf[64];
  htsmsg_t *sourceinfo;

  prop_t *audio_tracks, *subtitle_tracks, *p;
  media_codec_params_t mcp;

  if((hs = htsp_find_subscription_by_msg(hc, m)) == NULL)
    return;

  mp = hs->hs_mp;

  TRACE(TRACE_DEBUG, "HTSP", "Got start notitification");

  audio_tracks = prop_create(mp->mp_prop_metadata, "audiostreams");
  prop_destroy_childs(audio_tracks);

  subtitle_tracks = prop_create(mp->mp_prop_metadata, "subtitlestreams");
  prop_destroy_childs(subtitle_tracks);

  p = prop_create(audio_tracks, NULL);
  prop_set_string(prop_create(p, "title"), "Off");
  prop_set_string(prop_create(p, "id"), "audio:off");

  p = prop_create(subtitle_tracks, NULL);
  prop_set_string(prop_create(p, "title"), "Off");
  prop_set_string(prop_create(p, "id"), "sub:off");

  if((sourceinfo = htsmsg_get_map(m, "sourceinfo")) != NULL) {

    char format[256];

    snprintf(format, sizeof(format), "HTSP TV \"%s\" from \"%s\"",
	     htsmsg_get_str(sourceinfo, "service") ?: "<?>",
	     htsmsg_get_str(sourceinfo, "mux") ?: "<?>");

    prop_set_string(prop_create(mp->mp_prop_metadata, "format"), format);
  }

  /**
   * Parse each stream component and add it as a stream at our end
   */
  if((streams = htsmsg_get_list(m, "streams")) != NULL) {
    HTSMSG_FOREACH(f, streams) {
      if(f->hmf_type != HMF_MAP)
	continue;
      sub = &f->hmf_msg;

      if((type = htsmsg_get_str(sub, "type")) == NULL)
	continue;

      if(htsmsg_get_u32(sub, "index", &idx))
	continue;

      subid = 0;
      memset(&mcp, 0, sizeof(mcp));

      if(!strcmp(type, "AC3")) {
	codec_id = CODEC_ID_AC3;
	codec_type = CODEC_TYPE_AUDIO;
	nicename = "AC3";
	s = 3;
      } else if(!strcmp(type, "AAC")) {
	codec_id = CODEC_ID_AAC;
	codec_type = CODEC_TYPE_AUDIO;
	nicename = "AAC";
	s = 2;
      } else if(!strcmp(type, "MPEG2AUDIO")) {
	codec_id = CODEC_ID_MP2;
	codec_type = CODEC_TYPE_AUDIO;
	nicename = "MPEG";
	s = 1;
      } else if(!strcmp(type, "MPEG2VIDEO")) {
	codec_id = CODEC_ID_MPEG2VIDEO;
	codec_type = CODEC_TYPE_VIDEO;
	nicename = "MPEG-2";
	s = 1;
      } else if(!strcmp(type, "H264")) {
	codec_id = CODEC_ID_H264;
	codec_type = CODEC_TYPE_VIDEO;
	nicename = "H264";
	mcp.cheat_for_speed = 1;
	s = 2;
      } else if(!strcmp(type, "DVBSUB")) {
	codec_id = CODEC_ID_DVB_SUBTITLE;
	codec_type = CODEC_TYPE_SUBTITLE;
	nicename = "Subtitles";

	uint32_t composition_id, ancillary_id;
	
	if(htsmsg_get_u32(sub, "composition_id", &composition_id)) {
	  composition_id = 0;
	  TRACE(TRACE_ERROR, "HTSP", 
		"Subtitle stream #%d missing composition id", idx);
	}

	if(htsmsg_get_u32(sub, "ancillary_id", &ancillary_id)) {
	  ancillary_id = 0;
	  TRACE(TRACE_ERROR, "HTSP", 
		"Subtitle stream #%d missing ancillary id", idx);
	}

	mcp.sub_id =
	  (composition_id & 0xffff) | ((ancillary_id & 0xffff) << 16);
	s = 1;

      } else {
	continue;
      }

      htsmsg_get_u32(sub, "width", &mcp.width);
      htsmsg_get_u32(sub, "height", &mcp.height);

      /**
       * Try to create the codec
       */

      cw = media_codec_create(codec_id, codec_type, 0, NULL, NULL, &mcp, mp);
      if(cw == NULL) {
	TRACE(TRACE_ERROR, "HTSP", "Unable to create codec for %s (#%d)",
	      nicename, idx);
	continue; /* We should print something i guess .. */
      }

      hss = calloc(1, sizeof(htsp_subscription_stream_t));
      hss->hss_index = idx;
      hss->hss_cw = cw;

      title = htsmsg_get_str(sub, "language");
      if(title == NULL) {
	snprintf(titlebuf, sizeof(titlebuf), "Stream %d", idx);
	title = titlebuf;
      }

      switch(codec_type) {
      default:
	break;

      case CODEC_TYPE_VIDEO:
	hss->hss_mq = &mp->mp_video;
	hss->hss_data_type = MB_VIDEO;

	if(s > vscore) {
	  vscore = s;
	  vstream = idx;
	}
	metaparent = NULL;
	break;

      case CODEC_TYPE_SUBTITLE:
	hss->hss_mq = &mp->mp_video;
	hss->hss_data_type = MB_SUBTITLE;

	if(s > sscore) {
	  sscore = s;
	  sstream = idx;
	}
	metaparent = prop_create(subtitle_tracks, NULL);
	prop_set_stringf(prop_create(metaparent, "id"), "sub:%d", idx);
	break;

      case CODEC_TYPE_AUDIO:
	hss->hss_mq = &mp->mp_audio;
	hss->hss_data_type = MB_AUDIO;
	
	if(s > ascore) {
	  ascore = s;
	  astream = idx;
	}
	metaparent = prop_create(audio_tracks, NULL);
	prop_set_stringf(prop_create(metaparent, "id"), "audio:%d", idx);
	break;
      }

      TRACE(TRACE_DEBUG, "HTSP", "Stream #%d: %s", idx, title);

      LIST_INSERT_HEAD(&hs->hs_streams, hss, hss_link);


      if(metaparent != NULL) {
	prop_set_string(prop_create(metaparent, "title"), title);
	prop_set_string(prop_create(metaparent, "format"), nicename);
      }
    }
  }
  mp->mp_audio.mq_stream  = astream;
  mp->mp_video.mq_stream  = vstream;
  mp->mp_video.mq_stream2 = sstream;

  if(astream != -1)
    prop_set_stringf(mp->mp_prop_audio_track_current, "audio:%d", astream);
  else
    prop_set_string(mp->mp_prop_audio_track_current, "audio:off");

  if(sstream != -1)
    prop_set_stringf(mp->mp_prop_subtitle_track_current, "sub:%d", sstream);
  else
    prop_set_string(mp->mp_prop_subtitle_track_current, "sub:off");

  TRACE(TRACE_DEBUG, "HTSP", "Selecting Video-stream:%d, Audio-stream: %d, "
	"Subtitle-stream: %d",
	vstream, astream, sstream);

  hts_mutex_unlock(&hc->hc_subscription_mutex);
}


/**
 *
 */
static void
htsp_subscriptionStop(htsp_connection_t *hc, htsmsg_t *m)
{
  htsp_subscription_t *hs;

  if((hs = htsp_find_subscription_by_msg(hc, m)) == NULL)
    return;
  TRACE(TRACE_DEBUG, "HTSP", "Subscription stopped");

  htsp_free_streams(hs);
  hts_mutex_unlock(&hc->hc_subscription_mutex);
}


/**
 *
 */
static void
htsp_subscriptionStatus(htsp_connection_t *hc, htsmsg_t *m)
{
  htsp_subscription_t *hs;
  const char *status = htsmsg_get_str(m, "status");
    
  if((hs = htsp_find_subscription_by_msg(hc, m)) == NULL)
    return;

  prop_set_string(prop_create(hs->hs_mp->mp_prop_root, "error"), status);

  if(status != NULL)
    TRACE(TRACE_ERROR, "HTSP", "%s", status);

  hts_mutex_unlock(&hc->hc_subscription_mutex);
}


/**
 *
 */
static void
htsp_queueStatus(htsp_connection_t *hc, htsmsg_t *m)
{
  htsp_subscription_t *hs;
  uint32_t u32;
  uint32_t drops;
  prop_t *r;
  media_pipe_t *mp;

  if((hs = htsp_find_subscription_by_msg(hc, m)) == NULL)
    return;

  mp = hs->hs_mp;

  drops = 0;
  if(!htsmsg_get_u32(m, "Bdrops", &u32))
    drops += u32;
  if(!htsmsg_get_u32(m, "Pdrops", &u32))
    drops += u32;
  if(!htsmsg_get_u32(m, "Idrops", &u32))
    drops += u32;

  prop_set_int(prop_create(mp->mp_prop_root, "isRemote"), 1);

  r = prop_create(mp->mp_prop_root, "remote");
  prop_set_int(prop_create(r, "drops"), drops);

  prop_set_int(prop_create(r, "qlen"),
	       htsmsg_get_u32_or_default(m, "packets", 0));

  prop_set_int(prop_create(r, "qbytes"),
	       htsmsg_get_u32_or_default(m, "bytes", 0));

  hts_mutex_unlock(&hc->hc_subscription_mutex);
}



/**
 *
 */
static int
be_htsp_canhandle(backend_t *be, const char *url)
{
  return !strncmp(url, "htsp://", strlen("htsp://"));
}


/**
 *
 */
static int
htsp_init(void)
{
  hts_mutex_init(&htsp_global_mutex);
  return 0;
}



/**
 *
 */
static backend_t be_htsp = {
  .be_init       = htsp_init,
  .be_canhandle  = be_htsp_canhandle,
  .be_open       = be_htsp_open,
  .be_play_video = be_htsp_playvideo,
};

BE_REGISTER(htsp);
