
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
#define MAX_DATA_SIZE 500
#define d 0

int packetsInFlight[500]={0};

struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */
  //for sending side
  int lastByteAcked;
  int lastByteSent;
  int lastByteWritten;
  uint32_t seqNum;
  uint32_t nextAckNum;
  
  char *sendingWindow;

  //for receiving side
  int lastByteRead;
  int nextByteExpected;
  int lastByteReceived;
  char *receivingWindow;
  uint32_t nextSeqNum;
  uint32_t lastSeqNum;

  int maxWindowSize;
  
  
  
  

};
rel_t *rel_list;

//declared helper functions
int getSendBufferSize(rel_t *r);
void makePacket(rel_t *s, struct packet *p, int sizeOfData);
void allignSendingWindow(rel_t *r);
void allignReceivingWindow(rel_t *r);
void makeAckPacket(rel_t *s, struct packet *p);
int getReceiveBufferSize(rel_t *r);
void rel_output2(rel_t *r, int lenToPrint);
void sendAck(rel_t *r);
int checkDestroy(rel_t *r);




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

  //ss is null, so this is a client or both. do setup for both client and serer
  if(ss==NULL){
    r->maxWindowSize=(cc->window * MAX_DATA_SIZE);
    r->sendingWindow=(char*)malloc(r->maxWindowSize * sizeof(char));
    
    r->receivingWindow=(char*)malloc(r->maxWindowSize * sizeof(char));
    r->nextByteExpected=1;
  }

  //do server setup
  else{
    r->maxWindowSize=cc->window * MAX_DATA_SIZE;
    r->receivingWindow=(char*)malloc(r->maxWindowSize * sizeof(char));
    r->nextByteExpected=1;
  }
  r->seqNum=1; r->nextAckNum=1; r->nextSeqNum=1;

  return r;
}

void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  free(r->sendingWindow);
  free(r->receivingWindow);
  free(r);

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
  printf("%d\n", 17);
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
  

  //ack packet
  if(ntohs(pkt->len)==8){
    if(d==1)
      printf("received ack packet\n");
    //an ack greater than or equal to the next expected ack number has arrived!
    if(ntohl(pkt->ackno) >= r->nextAckNum){
      int j;
      //this loop is so that we can move the window even if the ack will cause multiple sent packets to be acked
      for(j=0; j<ntohl(pkt->ackno) - r->nextAckNum+1; j++){
          r->lastByteAcked+=MAX_DATA_SIZE;
          allignSendingWindow(r);

      }
      //now we are waiting for the next ack
      r->nextAckNum=ntohl(pkt->ackno)+1;
    }
  }
  //dataPacket
  else{
    if(d==1)
      printf("received data packet of length %d: %s\n", ntohs(pkt->len),pkt->data);
    //checksum isn't the same. Just drop the packet
    // if(cksum(pkt->data,pkt->len) != pkt->cksum){
    //   printf("hi\n");
    //   int c = pkt->cksum;
    //   pkt->cksum=0;
    //   // printf("they are %d %d", c, cksum(pkt, pkt->len));
    //   // if(d==1)printf("checksum failed!\n");
    //   // return;
    // }

    //if we've received an EOF, check if we should destroy the connection
    if(pkt->len == 12){
      checkDestroy(r);
      return;
    }
    //if we've already acked this packet, our ack was lost so ack it again!
    if(ntohl(pkt->seqno)<r->nextSeqNum){
      if(d==1)printf("reacking cause ack was lost\n");
      sendAck(r);
      return;
    }
    //If you have room for it in the TCP buffer, store it
    if(getReceiveBufferSize(r)>=MAX_DATA_SIZE){

      int i;
      //this loop is so there is room left if this packet was received out of order. If it was in order, this for loop won't do anything
      for(i=0; i<(ntohl(pkt->seqno) - r->nextSeqNum); i++){
        r->lastByteReceived+=MAX_DATA_SIZE;

      }
      //store it at lastbytereceived
      strcpy(r->receivingWindow+ r->lastByteReceived,pkt->data);
      //increment last byte received by a max packet
      r->lastByteReceived+=MAX_DATA_SIZE;
      //if we received this in order, move nextByteExpected. also move the nextSeqNum
      //to do, decide if we have future segments so we can write multiple things!
      if(ntohl(pkt->seqno) == r->nextSeqNum){
        r->nextByteExpected+=MAX_DATA_SIZE;
        r->nextSeqNum+=1;

        //everything is in order, write to output and possibly send acks if we can write!
        rel_output2(r, ntohs(pkt->len)-12);
      }
    }    
    
  }
}


void
rel_read (rel_t *s)
{
  int isData;

  //loop while you still have buffer space to store data
  while(getSendBufferSize(s)>0){
    //read data into the sending window at the end

    isData = conn_input(s->c, s->sendingWindow+s->lastByteWritten,getSendBufferSize(s));

    
    //there is no data to receive
    if(isData==0)
      return;
    //you received an error
    if(isData==-1){
      //check if packet should be destroyed
      checkDestroy(s);
      return;
    }
    //update the lastByteWritten by the size of one packet
    s->lastByteWritten+=MAX_DATA_SIZE;
    
    // if(d==1)
      // printf("Packet sent %s with size %d and new sw looks like %s with size of %d\n",s->sendingWindow+s->lastByteWritten-MAX_DATA_SIZE,isData,s->sendingWindow, getSendBufferSize(s) );

    struct packet *p = (struct packet*)malloc(sizeof(struct packet));
    makePacket(s,p, isData);
    conn_sendpkt(s->c, p, ntohs(p->len));
    if(d==1)
      printf("Packet sent %s with size %d\n",p->data,ntohs(p->len));

    s->lastByteSent+=MAX_DATA_SIZE;
    s->seqNum+=1;
    packetsInFlight[s->lastByteSent/500] = 0;
    free(p);

  }
}

void
rel_output (rel_t *r)
{
 rel_output2(r, MAX_DATA_SIZE); 

}

void rel_output2(rel_t *r, int lenToPrint){
  //there is no room to write!
  if(conn_bufspace(r->c)<lenToPrint)
      return;
  //there is nothing new to write
  if(r->lastByteWritten==r->nextByteExpected){
    return;
  }
  int success = conn_output(r->c, r->receivingWindow+r->lastByteRead, lenToPrint);
  //everything was printed, send an ack
  if(success == lenToPrint){
    sendAck(r);
    r->lastByteRead+=MAX_DATA_SIZE;
    allignReceivingWindow(r);

  }
}

void
rel_timer ()
{
  // printf("timer called\n");
  // int i;
  // for(i=0; i<lastByteSent/500+1; i++){

  // }

}

//get the amount of space left in the sender's buffer
int getSendBufferSize(rel_t *r){
  return r->maxWindowSize - (r->lastByteWritten-r->lastByteAcked);
}

//get the amount of space left in the receiver's buffer
int getReceiveBufferSize(rel_t *r){
  return r->maxWindowSize-((r->nextByteExpected-1)-r->lastByteRead);
}

void makePacket(rel_t *s, struct packet *p, int sizeOfData){
  strncpy(p->data,s->sendingWindow+s->lastByteWritten-MAX_DATA_SIZE, sizeOfData);
  p->data[sizeOfData]='\0';

  p->len = htons(sizeOfData+12);
  p->ackno=htonl(s->nextAckNum);
  p->seqno=htonl(s->seqNum);
  p->cksum=0;
  p->cksum = cksum(p, p->len);
  //to do use actual checksum
  
}

void makeAckPacket(rel_t *s, struct packet *p){
  p->len=htons(8);
  p->ackno = htonl(s->nextSeqNum-1);
  p->cksum = cksum(p,p->len); 
}

//When an ack is received we want to delete the data that was acked and move the sending window back
void allignSendingWindow(rel_t *r){
  memmove(r->sendingWindow, r->sendingWindow+MAX_DATA_SIZE, r->lastByteWritten-r->lastByteAcked);
  r->lastByteWritten-=MAX_DATA_SIZE;
  r->lastByteAcked-=MAX_DATA_SIZE;
  r->lastByteSent-=MAX_DATA_SIZE;
  
}

void allignReceivingWindow(rel_t *r){
    if(d==1)printf("receiving window alligned now %d %d %d\n", r->lastByteRead, r->nextByteExpected, r->lastByteReceived);

  memmove(r->receivingWindow, r->receivingWindow+MAX_DATA_SIZE, r->lastByteRead-r->lastByteReceived);
  r->lastByteRead-=MAX_DATA_SIZE;
  r->nextByteExpected-=MAX_DATA_SIZE;
  r->lastByteReceived-=MAX_DATA_SIZE;
}

//check whether a connection should be destroyed. this is only called if we get an EOF or if we get a -1 on read
int checkDestroy(rel_t *r){
  //you have no more packets to ack
  if(r->lastByteAcked == r->lastByteSent){
    //you have outputed all data
    if(r->lastByteRead==r->lastByteReceived){
      if(d==1)printf("chose to destroy!\n");
      rel_destroy(r);
      return 1;
    }
  }
  return 0;
}

void sendAck(rel_t *r){
  struct packet *p = (struct packet*)malloc(sizeof(struct packet));
  makeAckPacket(r, p);
  conn_sendpkt(r->c, p, p->len);
  free(p);
}


