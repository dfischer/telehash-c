#include "switch.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "util.h"

// a prime number for the internal hashtable used to track all active hashnames/lines
#define MAXPRIME 4211

switch_t switch_new(uint32_t prime)
{
  switch_t s;
  if(!(s = malloc(sizeof (struct switch_struct)))) return NULL;
  memset(s, 0, sizeof(struct switch_struct));
  s->cap = 256; // default cap size
  s->window = 32; // default reliable window size
  s->index = xht_new(prime?prime:MAXPRIME);
  s->parts = lob_new();
  s->active = bucket_new();
  s->tick = platform_seconds();
  if(!s->index || !s->parts) return switch_free(s);
  return s;
}

int switch_init(switch_t s, lob_t keys)
{
  int i = 0, err = 0;
  char *key, secret[10], csid, *pk, *sk;
  crypt_t c;

  while((key = lob_get_istr(keys,i)))
  {
    i += 2;
    if(strlen(key) != 2) continue;
    util_unhex((unsigned char*)key,2,(unsigned char*)&csid);
    sprintf(secret,"%s_secret",key);
    pk = lob_get_str(keys,key);
    sk = lob_get_str(keys,secret);
    DEBUG_PRINTF("loading pk %s sk %s",pk,sk);
    c = crypt_new(csid, (unsigned char*)pk, strlen(pk));
    if(crypt_private(c, (unsigned char*)sk, strlen(sk)))
    {
      err = 1;
      crypt_free(c);
      continue;
    }
    DEBUG_PRINTF("loaded %s",key);
    xht_set(s->index,(const char*)c->csidHex,(void *)c);
    lob_set_str(s->parts,c->csidHex,c->part);
  }
  
  lob_free(keys);
  if(err || !s->parts->json)
  {
    DEBUG_PRINTF("key loading failed");
    return 1;
  }
  s->id = hashname_getparts(s->index, s->parts);
  if(!s->id) return 1;
  return 0;
}

switch_t switch_free(switch_t s)
{
  if(!s) return NULL;
  xht_free(s->index);
  lob_free(s->parts);
  if(s->seeds) bucket_free(s->seeds);
  bucket_free(s->active);
  free(s);
  return NULL;
}

void switch_capwin(switch_t s, int cap, int window)
{
  s->cap = cap;
  s->window = window;
}

// generate a broadcast/handshake ping packet
lob_t switch_ping(switch_t s)
{
  char *key, rand[8], trace[17];
  int i = 0;
  lob_t p = lob_new();
  lob_set_str(p,"type","ping");
  while((key = lob_get_istr(s->parts,i)))
  {
    i += 2;
    if(strlen(key) != 2) continue;
    lob_set(p,key,"true",4);
  }
  // get/gen token to validate pongs
  if(!(key = xht_get(s->index,"ping")))
  {
    xht_store(s->index,"ping",util_hex(crypt_rand((unsigned char*)rand,8),8,(unsigned char*)trace),17);
    key = trace;
  }
  lob_set_str(p,"trace",key);
  return p;
}

// caller must ensure it's ok to send a pong, and is responsible for ping and in
void switch_pong(switch_t s, lob_t ping, path_t in)
{
  char *key;
  unsigned char hex[3], csid = 0, best = 0;
  int i = 0;
  lob_t p;
  crypt_t c;

  // check our parts for the best match  
  while((key = lob_get_istr(s->parts,i)))
  {
    i += 2;
    if(strlen(key) != 2) continue;
    util_unhex((unsigned char*)key,2,(unsigned char*)&csid);
    if(csid <= best) continue;
    best = csid;
    memcpy(hex,key,3);
  }

  if(!best || !(c = xht_get(s->index,(char*)hex))) return;
  p = lob_new();
  lob_set_str(p,"type","pong");
  lob_set_str(p,"trace",lob_get_str(ping,"trace"));
  lob_set(p,"from",(char*)s->parts->json,s->parts->json_len);
  lob_body(p,c->key,c->keylen);
  p->out = path_copy(in);
  switch_sendingQ(s,p);
}

// fire tick events no more than once a second
void switch_loop(switch_t s)
{
  hashname_t hn;
  int i = 0;
  uint32_t now = platform_seconds();
  if(s->tick == now) return;
  s->tick = now;

  // give all channels a tick
  while((hn = bucket_get(s->active,i)))
  {
    i++;
    chan_tick(s,hn);
  }
}

void switch_seed(switch_t s, hashname_t hn)
{
  if(!s->seeds) s->seeds = bucket_new();
  bucket_add(s->seeds, hn);
}

lob_t switch_sending(switch_t s)
{
  lob_t p;
  if(!s) return NULL;
  // run a loop just in case to flush any outgoing
  switch_loop(s);
  if(!s->out) return NULL;
  p = s->out;
  s->out = p->next;
  if(!s->out) s->last = NULL;
  return p;
}

// internally adds to sending queue
void switch_sendingQ(switch_t s, lob_t p)
{
  lob_t dup;
  if(!p) return;

  // if there's no path, find one or copy to many
  if(!p->out)
  {
    // just being paranoid
    if(!p->to)
    {
      lob_free(p);
      return;
    }

    // if the last path is alive, just use that
    if(path_alive(p->to->last)) p->out = p->to->last;
    else{
      int i;
      // try sending to all paths
      for(i=0; p->to->paths[i]; i++)
      {
        dup = lob_copy(p);
        dup->out = p->to->paths[i];
        switch_sendingQ(s, dup);
      }
      lob_free(p);
      return;   
    }
  }

  // update stats
  p->out->tout = platform_seconds();

  // add to the end of the queue
  if(s->last)
  {
    s->last->next = p;
    s->last = p;
    return;
  }
  s->last = s->out = p;  
}

// tries to send an open if we haven't
void switch_open(switch_t s, hashname_t to, path_t direct)
{
  lob_t open, inner;

  if(!to) return;
  if(!to->c)
  {
    DEBUG_PRINTF("can't open, no key for %s",to->hexname);
    if(s->handler) s->handler(s,to);
    return;
  }

  // actually send the open
  inner = lob_new();
  lob_set_str(inner,"to",to->hexname);
  lob_set(inner,"from",(char*)s->parts->json,s->parts->json_len);
  open = crypt_openize((crypt_t)xht_get(s->index,to->hexid), to->c, inner);
  DEBUG_PRINTF("opening to %s %hu %s",to->hexid,lob_len(open),to->hexname);
  if(!open) return;
  open->to = to;
  if(direct) open->out = direct;
  switch_sendingQ(s, open);
}

void switch_send(switch_t s, lob_t p)
{
  lob_t lined;

  if(!p) return;
  
  // require recipient at least, and not us
  if(!p->to || p->to == s->id) return (void)lob_free(p);

  // encrypt the packet to the line, chains together
  lined = crypt_lineize(p->to->c, p);
  if(lined) return switch_sendingQ(s, lined);

  // no line, so generate open instead
  switch_open(s, p->to, NULL);
}

chan_t switch_pop(switch_t s)
{
  chan_t c;
  if(!s->chans) return NULL;
  c = s->chans;
  chan_dequeue(c);
  return c;
}

void switch_receive(switch_t s, lob_t p, path_t in)
{
  hashname_t from;
  lob_t inner;
  crypt_t c;
  chan_t chan;
  char hex[3];
  char lineHex[33];

  if(!s || !p || !in) return;

  // handle open packets
  if(p->json_len == 1)
  {
    util_hex(p->json,1,(unsigned char*)hex);
    c = xht_get(s->index,hex);
    if(!c) return (void)lob_free(p);
    inner = crypt_deopenize(c, p);
    if(!inner) return (void)lob_free(p);

    from = hashname_frompacket(s->index, inner);
    if(crypt_line(from->c, inner) != 0) return; // not new/valid, ignore
    
    // line is open!
    DEBUG_PRINTF("line in %d %s %d %s",from->c->lined,from->hexname,from,from->c->lineHex);
    xht_set(s->index, (const char*)from->c->lineHex, (void*)from);
    in = hashname_path(from, in, 1);
    switch_open(s, from, in); // in case we need to send an open
    // this resends any opening channel packets
    if(from->c->lined == 1) chan_reset(s, from);
    return;
  }

  // handle line packets
  if(p->json_len == 0)
  {
    util_hex(p->body, 16, (unsigned char*)lineHex);
    from = xht_get(s->index, lineHex);
    if(from)
    {
      p = crypt_delineize(from->c, p);
      if(!p)
      {
        DEBUG_PRINTF("invlaid line from %s %s",path_json(in),from->hexname);
        return;
      }
      in = hashname_path(from, in, 1);
      
      // route to the channel
      if((chan = chan_in(s, from, p)))
      {
        // if new channel w/ seq, configure as reliable
        if(!chan->opened && lob_get_str(p,"seq")) chan_reliable(chan, s->window);
        return chan_receive(chan, p);
      }

      // drop unknown!
      DEBUG_PRINTF("dropping unknown channel packet %.*s",p->json_len,p->json);
      lob_free(p);
      return;
    }
  }

  // handle valid pong responses, start handshake
  if(util_cmp("pong",lob_get_str(p,"type")) == 0 && util_cmp(xht_get(s->index,"ping"),lob_get_str(p,"trace")) == 0 && (from = hashname_fromjson(s->index,p)) != NULL)
  {
    DEBUG_PRINTF("pong from %s",from->hexname);
    in = hashname_path(from, in, 0);
    switch_open(s,from,in);
    lob_free(p);
    return;
  }

  // handle pings, respond if local only or dedicated seed
  if(util_cmp("ping",lob_get_str(p,"type")) == 0 && (s->isSeed || path_local(in)))
  {
    switch_pong(s,p,in);
    lob_free(p);
    return;
  }

  // nothing processed, clean up
  lob_free(p);
}

// sends a note packet to it's channel if it can, !0 for error
int switch_note(switch_t s, lob_t note)
{
  chan_t c;
  lob_t notes;
  if(!s || !note) return -1;
  c = xht_get(s->index,lob_get_str(note,".to"));
  if(!c) return -1;
  notes = c->notes;
  while(notes) notes = notes->next;
  if(!notes) c->notes = note;
  else notes->next = note;
  chan_queue(c);
  return 0;

}
