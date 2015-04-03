
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
#include <pthread.h>

#include "rlib.h"
#define MAX_DATA_SIZE 500
#define d 2

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
  uint32_t seqNum; //the sequence number you should send starting at 1
  uint32_t nextAckNum; //the next ack number that you expect
  
  char *sendingWindow;

  //for receiving side
  int lastByteRead;
  int nextByteExpected;
  int lastByteReceived;
  char *receivingWindow;
  uint32_t nextSeqNum; //the next sequence number the receiver expects

  int maxWindowSize;


  int id;
  int timeout;
};
rel_t *rel_list;

struct packet_info{
  rel_t *r;
  int size;
  int ackNo;
  int seqNo;
};

int readEOF;
int recEOF;

//declared helper functions
int getSendBufferSize(rel_t *r);
void makePacket(rel_t *s, struct packet *p, int sizeOfData, int seqno);
void allignSendingWindow(rel_t *r);
void allignReceivingWindow(rel_t *r);
void makeAckPacket(rel_t *s, struct packet *p);
int getReceiveBufferSize(rel_t *r);
void rel_output2(rel_t *r, int lenToPrint);
void sendAck(rel_t *r);
int checkDestroy(rel_t *r);
void *timer(void *vargp);
void sendPacket(rel_t *s, int isData, int seqNo);




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
    fprintf(stderr, "nooooo\n");
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
  if(d==2)fprintf(stderr, "window size: %d\n", cc->window);
  r->maxWindowSize=(cc->window * MAX_DATA_SIZE);
  r->sendingWindow=(char*)malloc(r->maxWindowSize * sizeof(char));
  r->receivingWindow=(char*)malloc(r->maxWindowSize * sizeof(char));
  r->nextByteExpected=1;
  r->seqNum=1; r->nextAckNum=2; r->nextSeqNum=1;
  srand(time(NULL));
  r->id = rand();
  r->timeout = cc->timeout;
  return r;
}

void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);
  if(d==1)fprintf(stderr, "connection destroyed!\n");

  free(r->sendingWindow);
  free(r->receivingWindow);
  free(r);

}

void
rel_demux (const struct config_common *cc,
  const struct sockaddr_storage *ss,
  packet_t *pkt, size_t len)
{
  fprintf(stderr, "noooo\n");
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
  if(d==1)fprintf(stderr, "random n variable: %d pkt len: %d\n", n, ntohs(pkt->len));
  //checksum isn't the same. Just drop the packet
  uint16_t tempsum = pkt->cksum;
  pkt->cksum=0;
  if(cksum(pkt,ntohs(pkt->len)) != tempsum){      
    if(d==1)fprintf(stderr,"checksum failed!\n");
    return;
  }

  //ack packet
  if(ntohs(pkt->len)==8){
    if(d==1)fprintf(stderr,"id %d received ack packet %d\n", r->id,ntohl(pkt->ackno));
    checkDestroy(r);

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
      rel_read(r);
    }
  }
  //dataPacket
  else{
    if(d==1)
      if(d==1)fprintf(stderr,"id %d received data packet of length %d seq %d\n", r->id,ntohs(pkt->len), ntohl(pkt->seqno));

    //if we've received an EOF, check if we should destroy the connection
    if(ntohs(pkt->len) == 12){
      if(d==1)fprintf(stderr, "eof received\n");
      r->nextSeqNum+=1;
      sendAck(r);
      recEOF=1;
      checkDestroy(r);
      // rel_read(r);
      return;
    }
    //if we've already acked this packet, our ack was lost so ack it again!
    if(ntohl(pkt->seqno)<r->nextSeqNum){
      if(d==1)fprintf(stderr,"reacking cause ack was lost\n");
      sendAck(r);
      return;
    }
    //If you have room for it in the TCP buffer, store it
    if(getReceiveBufferSize(r)>=MAX_DATA_SIZE){

      int i;
      //this loop is so there is room left if this packet was received out of order. If it was in order, this for loop won't do anything
      for(i=0; i<(ntohl(pkt->seqno) - r->nextSeqNum); i++){
        r->lastByteReceived+=MAX_DATA_SIZE;
        if(d==1)fprintf(stderr,"space put in receiving window\n");

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
    else{
      fprintf(stderr, "packet dropped cause no room\n");
    }    
    
  }
}
int num;

void
rel_read (rel_t *s)
{
  int isData;
  if(readEOF==1)return;
  // if(s->nextAckNum==s->seqNum)return;
  if(d==1)fprintf(stderr, "reading\n");

  //loop while you still have buffer space to store data
  while(getSendBufferSize(s)>0){

    //read data into the sending window at the end
    char *temp=(char*)malloc(s->maxWindowSize * sizeof(char));
    //isData = conn_input(s->c, s->sendingWindow+s->lastByteWritten,getSendBufferSize(s));

    isData = conn_input(s->c, temp,getSendBufferSize(s));
     //you received an error or EOF

    if(isData==-1){
      if(d==1)fprintf(stderr, "read EOF\n");
      //send EOF to receiver
      readEOF=1;
      sendPacket(s, 0, s->seqNum);
      // sendAck(s);
      free(temp);
      //s->lastByteWritten+=MAX_DATA_SIZE;

      return;
    }
    
    //there is no data to receive
    if(isData==0){
      if(d==1)fprintf(stderr, "nothing to read\n");
      free(temp);
      return;
    }
    num+=1;
    if(d==1)fprintf(stderr, "number of reads: %d\n", num);
    if(d==1)fprintf(stderr, "Something read\n" );

   
    strncpy(s->sendingWindow+s->lastByteWritten, temp, isData);
    free(temp);
    s->lastByteWritten+=MAX_DATA_SIZE;


    //update the lastByteWritten by the size of one packet

    // if(d==1)
      // printf("Packet sent %s with size %d and new sw looks like %s with size of %d\n",s->sendingWindow+s->lastByteWritten-MAX_DATA_SIZE,isData,s->sendingWindow, getSendBufferSize(s) );
    sendPacket(s, isData, s->seqNum);


    s->lastByteSent+=MAX_DATA_SIZE;
    s->seqNum+=1;


  }
}

void
rel_output (rel_t *r)
{
  fprintf(stderr, "in output\n");
 rel_output2(r, 500); 

}

void rel_output2(rel_t *r, int lenToPrint){
  //there is no room to write!
  if(conn_bufspace(r->c)<lenToPrint)
    return;
  //there is nothing new to write
  if(r->lastByteRead==r->nextByteExpected){
    fprintf(stderr, "nothing new to write\n");
    return;
  }

  int success = conn_output(r->c, r->receivingWindow+r->lastByteRead, lenToPrint);
  //everything was printed, send an ack
  if(success == lenToPrint){
    if(d==1)fprintf(stderr, "outputted\n");
    sendAck(r);
    r->lastByteRead+=MAX_DATA_SIZE;
    allignReceivingWindow(r);
    
  }
  else{
    fprintf(stderr, "not outputed\n");
  }
  // rel_read(r);
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

void makePacket(rel_t *s, struct packet *p, int sizeOfData, int seqno){
  //p->data=char [sizeOfData+1];

  strncpy(p->data,s->sendingWindow+s->lastByteWritten-MAX_DATA_SIZE, sizeOfData);
  p->len = htons(sizeOfData+12);
  p->ackno=htonl(s->nextAckNum);
  p->seqno=htonl(seqno);
  p->cksum=0;
  p->cksum = cksum(p, ntohs(p->len));
  
}

void makeAckPacket(rel_t *s, struct packet *p){
  p->len=htons(8);
  p->ackno = htonl(s->nextSeqNum);
  p->cksum=0;
  p->cksum = cksum(p,ntohs(p->len)); 
}

//When an ack is received we want to delete the data that was acked and move the sending window back
void allignSendingWindow(rel_t *r){
  //first, delete data that was acked
  memset(r->sendingWindow, 0, MAX_DATA_SIZE);
  //then move rest of data back. If window is 1, this won't do anything.
  if(d==1)fprintf(stderr, "%d\n",r->lastByteWritten-r->lastByteAcked);
  memmove(r->sendingWindow, r->sendingWindow+MAX_DATA_SIZE, r->lastByteWritten-r->lastByteAcked);
  r->lastByteWritten-=MAX_DATA_SIZE;
  r->lastByteAcked-=MAX_DATA_SIZE;
  r->lastByteSent-=MAX_DATA_SIZE;
        // if(d==1)fprintf(stderr,"sending window alligned now %d %d %d\n", r->lastByteAcked, r->lastByteSent, r->lastByteWritten);
}

void allignReceivingWindow(rel_t *r){
    // if(d==1)fprintf(stderr,"receiving window alligned now %d %d %d\n", r->lastByteRead, r->nextByteExpected, r->lastByteReceived);
  //first, delete what we just wrote
  memset(r->receivingWindow, 0, MAX_DATA_SIZE);
  //then move rest of data back. If window is 1, this won't do anything.
  memmove(r->receivingWindow, r->receivingWindow+MAX_DATA_SIZE, r->lastByteRead-r->lastByteReceived);
  r->lastByteRead-=MAX_DATA_SIZE;
  r->nextByteExpected-=MAX_DATA_SIZE;
  r->lastByteReceived-=MAX_DATA_SIZE;

}

//check whether a connection should be destroyed. this is only called if we get an EOF or if we get a -1 on read
int checkDestroy(rel_t *r){
  if(d==1)fprintf(stderr, "checking to destroy %d %d\n", recEOF, readEOF);
  //you have no more packets to ack
  if(readEOF==1 && recEOF==1){
    fprintf(stderr, "first two\n");
    if(r->lastByteAcked == r->lastByteSent){
      //you have outputed all data
      if(r->lastByteRead==r->lastByteReceived){
        if(d==1)fprintf(stderr,"chose to destroy!\n");
        rel_destroy(r);
        return 1;
      }
    }
  }
  return 0;
}

void sendAck(rel_t *r){
  struct packet *p = (struct packet*)malloc(sizeof(struct packet));
  makeAckPacket(r, p);
  conn_sendpkt(r->c, p, ntohs(p->len));
  if(d==1)fprintf(stderr, "id %d sent ack %d size %d\n", r->id, r->nextSeqNum,ntohs(p->len));

  free(p);
}

//send a data packet
void sendPacket(rel_t *s, int dataSize, int seqNo){

  struct packet *p = (struct packet*)malloc(sizeof(struct packet));
  makePacket(s,p, dataSize, seqNo);

  struct packet_info *pinfo = (struct packet_info*)malloc(sizeof(struct packet_info));

  // r = xmalloc (sizeof (*r));
  // memset (r, 0, sizeof (*r));

  // memcpy(r,s, sizeof(rel_t));
  rel_t *r;
  // pinfo->r=malloc (sizeof (*r));
  // memset (pinfo->r, 0, sizeof (*r));
  pinfo->r = s;
  pinfo->size=dataSize;
  pinfo->ackNo = s->nextAckNum;
  pinfo->seqNo = seqNo;

  pthread_t tid;
  pthread_create(&tid, NULL, timer,(void *) pinfo);


  conn_sendpkt(s->c, p, ntohs(p->len));
  if(d==1)
    fprintf(stderr,"Packet sent from %d with size %d and seqno %d\n",s->id,ntohs(p->len), seqNo);  

  free(p);
}


void *timer(void *vargp){
  struct packet_info *pinfo = (struct packet_info*) vargp;
  int prevAckNum = pinfo->ackNo;
  int size = pinfo->size;
  rel_t *r = pinfo->r;
  int seqNo = pinfo->seqNo;
  // free(pinfo->r);
  free(pinfo);

  sleep(r->timeout/1000);
  if(prevAckNum == r->nextAckNum){
    if(d==1)fprintf(stderr, "need to retransmit! %d\n", seqNo);
    sendPacket(r,size, seqNo);
  }
  return NULL;
}