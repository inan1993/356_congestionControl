
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"



struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */
  //for client
  int lastByteAcked;
  int lastByteSent;
  int lastByteWritten;
  char *sendingWindow;

  //for server
  int lastByteRead;
  int nextByteExpected;
  int lastByteReceived;
  char *receivingWindow;

  //for both
  int maxWindowSize;

};
rel_t *rel_list;

//declared helper functions
int getSendBufferSize(rel_t *r);




/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */


rel_t * rel_create (conn_t *c, const struct sockaddr_storage *ss,
	    const struct config_common *cc)
{
  rel_t *r;

  r = xmalloc (sizeof (*r));
  memset (r, 0, sizeof (*r));

  if (!c) {
    c = conn_create (r, ss);
    if (!c) {
      free (r);
      return NULL;
    }
  }

  r->c = c;
  r->next = rel_list;
  r->prev = &rel_list;
  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  //ss is null, so this is a client. do client setup
  if(ss==NULL){
    r->maxWindowSize=cc->window;
    r->sendingWindow=(char*)malloc(r->maxWindowSize * sizeof(char));
  }

  //do server setup
  else{
    r->maxWindowSize=cc->window;
    r->receivingWindow=(char*)malloc(r->maxWindowSize * sizeof(char));
  }

  return r;
}

void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  /* Free any other allocated memory here */
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
	   const struct sockaddr_storage *ss,
	   packet_t *pkt, size_t len)
{
  printf("%d\n", pkt->seqno);
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
}


void
rel_read (rel_t *s)
{
  int isData;

  //loop while you still have buffer space to store data
  while(getSendBufferSize(s)>0){
    //read data into the sending window at the end

    isData = conn_input(s->c, s->sendingWindow+s->lastByteWritten,getSendBufferSize(s));

    //update the lastByteWritten by the size of the new data
    s->lastByteWritten+=isData;
    //there is no data to receive
    if(isData==0)
      return;
    //you received an error
    if(isData==-1){
      //check that all acks are received. idk how to do this now
      rel_destroy(s);
      return;
    }

    
    printf("Data read is %s with size %d and new sw looks like %s with size of %d\n",s->sendingWindow+s->lastByteWritten-isData,isData,s->sendingWindow, getSendBufferSize(s) );

  }
}

void
rel_output (rel_t *r)
{
}

void
rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */

}

//get the amount of space left in the sender's buffer
int getSendBufferSize(rel_t *r){
  return r->maxWindowSize - (r->lastByteWritten-r->lastByteAcked);
}
