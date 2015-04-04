
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
#define d 1


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
  


  //for receiving side
  int lastByteRead;
  int nextByteExpected;
  int lastByteReceived;
  //char *receivingWindow;
  uint32_t nextSeqNum; //the next sequence number the receiver expects

  int maxWindowSize;


  int id;
  int timeout;

  int packetsInFlight;
};
rel_t *rel_list;

struct packet_info{
  rel_t *r;
  int size;
  int ackNo;
  int seqNo;
  packet_t *p;
};

int readEOF;
int recEOF;

//declared helper functions
//int getSendBufferSize(rel_t *r);
void makePacket(rel_t *s, char *data, packet_t *p, int sizeOfData, int seqno);
//void allignSendingWindow(rel_t *r);
//void allignReceivingWindow(rel_t *r);
void makeAckPacket(rel_t *s, packet_t *p, int seqno);
//int getReceiveBufferSize(rel_t *r);
void rel_output2(rel_t *r, packet_t *p, int lenToPrint);
void sendAck(rel_t *r, packet_t *p);
int checkDestroy(rel_t *r);
void *timer(void *vargp);
void sendPacket(rel_t *s, packet_t *pkt, int seqNo);




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
  
  r->maxWindowSize=(cc->window * MAX_DATA_SIZE);
  //r->sendingWindow=(char*)malloc(r->maxWindowSize * sizeof(char));
  //r->receivingWindow=(char*)malloc(r->maxWindowSize * sizeof(char));
  r->nextByteExpected=1;
  r->seqNum=1; r->nextAckNum=2; r->nextSeqNum=1;
  srand(time(NULL));
  r->id = rand();
  r->timeout = cc->timeout;
  //sendAck(r);
  return r;
}

void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);
  fprintf(stderr, "connection destroyed!\n");

  //free(r->sendingWindow);
  //free(r->receivingWindow);
  //free(r);

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
  //checksum isn't the same. Just drop the packet
  uint16_t tempsum = pkt->cksum;
  pkt->cksum=0;
  if(cksum(pkt,ntohs(pkt->len)) != tempsum){      
    if(d==1)fprintf(stderr,"checksum failed!\n");
    return;
  }

  if(n!=(size_t)ntohs(pkt->len)){
    if(d==1)fprintf(stderr,"length check failed!\n");
    return;
  }

  //ack packet
  if(n==8){
    if(d==1)fprintf(stderr,"id %d received ack packet %d length %d\n", r->id,ntohl(pkt->ackno), ntohs(pkt->len));
    

    //an ack greater than or equal to the next expected ack number has arrived!
    //fprintf(stderr, "pack ackno: %d, nextAckNum: %d",ntohl(pkt->ackno), r->nextAckNum );
    if(ntohl(pkt->ackno) == r->nextAckNum){
      int j;
      //this loop is so that we can move the window even if the ack will cause multiple sent packets to be acked
      // for(j=0; j<ntohl(pkt->ackno) - r->nextAckNum+1; j++){
        // fprintf(stderr,"window aligned\n");
       // r->lastByteAcked+=MAX_DATA_SIZE;
        //allignSendingWindow(r);

      // }

      //now we are waiting for the next ack
      r->nextAckNum=ntohl(pkt->ackno)+1;
       r->packetsInFlight-=1;

      
    }
    checkDestroy(r);
    rel_read(r);
  }
  //dataPacket
  else{
    if(d==1)
      fprintf(stderr,"id %d received data packet of length %d seq %d\n", r->id,n, ntohl(pkt->seqno));

    //if we've received an EOF, check if we should destroy the connection
    if(n == 12){
      if(d==1)fprintf(stderr, "eof received\n");
      recEOF=1;
      conn_output (r->c, NULL, 0);
      //r->nextSeqNum+=1;
      sendAck(r, pkt);
      checkDestroy(r);
      return;
    }
    //if we've already acked this packet, our ack was lost so ack it again!
    if(ntohl(pkt->seqno)<r->nextSeqNum){
      if(d==1)fprintf(stderr,"reacking cause ack was lost\n");
      sendAck(r, pkt);
      return;
    }
    // if this is the next expected packet, output and ack
    if(ntohl(pkt->seqno)==r->nextSeqNum){
    	rel_output2(r,pkt, n-12);
    }

    else{
    	fprintf(stderr, "packet rcvd out of order and dropped\n");
    }



   /* //If you have room for it in the TCP buffer, store it
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
        rel_output2(r,pkt, n-12);
      }
    }
    else{
      fprintf(stderr, "packet dropped cause no room\n");
    }   */ 
    
  }
}


void
rel_read (rel_t *s)
{
  int isData;
  if(readEOF==1)return;
  // if(s->nextAckNum==s->seqNum)return;
  fprintf(stderr, "reading\n");

  //loop while you still have buffer space to store data
 // fprintf(stderr, "pif: %d \n",s->packetsInFlight );
  while(s->packetsInFlight < s->maxWindowSize/MAX_DATA_SIZE){

    //read data into the sending window at the end
    char *temp=(char*)malloc(s->maxWindowSize * sizeof(char));
    // isData = conn_input(s->c, s->sendingWindow+s->lastByteWritten,getSendBufferSize(s));
     //you received an error or EOF

    isData = conn_input(s->c, temp,s->maxWindowSize-(s->packetsInFlight * MAX_DATA_SIZE));

    if(isData==-1){
      fprintf(stderr, "read EOF\n");
      //send EOF to receiver
      readEOF=1;
      s->lastByteWritten+=MAX_DATA_SIZE;
      s->lastByteSent+=MAX_DATA_SIZE;

      struct packet *p = (struct packet*)malloc(sizeof(struct packet));
      makePacket(s, temp, p, 0, s->seqNum);

      sendPacket(s, p, s->seqNum);
      s->packetsInFlight+=1;
      return;
    }
    
    //there is no data to receive
    if(isData==0){
      fprintf(stderr, "nothing to read\n");
      //free(temp);
      return;
    }


    s->lastByteWritten+=MAX_DATA_SIZE;

    struct packet *p = (struct packet*)malloc(sizeof(struct packet));
    makePacket(s,temp,p, isData, s->seqNum);
    sendPacket(s, p, s->seqNum);
    s->packetsInFlight+=1;
	fprintf(stderr, "sendPackt\n");
    s->lastByteSent+=MAX_DATA_SIZE;
    s->seqNum+=1;
     // free(temp);



  }
}

void
rel_output (rel_t *r)
{
  fprintf(stderr, "in output\n");
 //rel_output2(r, 500); 

}

void rel_output2(rel_t *r, packet_t *pkt, int lenToPrint){
  //there is no room to write!
  if(conn_bufspace(r->c)<lenToPrint){
  	fprintf(stderr, "not enough room to write\n");
    return;
	}

  int success = conn_output(r->c, pkt->data, lenToPrint);
  //everything was printed, send an ack
  if(success == lenToPrint){
    fprintf(stderr, "outputted %s\n", pkt->data);
    sendAck(r, pkt);
    r->nextSeqNum+=1;
    
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


void makePacket(rel_t *s, char *data, struct packet *p, int sizeOfData, int seqno){
  strncpy(p->data,data, sizeOfData);
//  p->data[sizeOfData]='\0';
  p->len = htons(sizeOfData+12);
  p->ackno=htonl(s->nextAckNum);
  p->seqno=htonl(seqno);
  p->cksum=0;
  p->cksum = cksum(p, ntohs(p->len));
  
}

void makeAckPacket(rel_t *s, struct packet *p, int seqno){
  p->len=htons(8);
  p->ackno = htonl(seqno+1);
  p->cksum=0;
  p->cksum = cksum(p,ntohs(p->len)); 
}


//check whether a connection should be destroyed. this is only called if we get an EOF or if we get a -1 on read
int checkDestroy(rel_t *r){
  fprintf(stderr, "checking to destroy %d %d\n", recEOF, readEOF);
  if(readEOF==1 && recEOF==1){
    fprintf(stderr, "first two\n");
    //you have no more packets to ack
    //fprintf(stderr, "lba: %d lbs: %d",r->lastByteAcked, r->lastByteSent );
    fprintf(stderr, "%d\n", r->packetsInFlight );
    if(r->packetsInFlight==0){
      //you have outputed all data
      //fprintf(stderr, "lbread: %d lbreceived: %d",r->lastByteRead, r->lastByteReceived );

      //if(r->lastByteRead==r->lastByteReceived){
        if(d==1)fprintf(stderr,"chose to destroy!\n");
        rel_destroy(r);
        return 1;
      //}
    }
  }
  return 0;
}

void sendAck(rel_t *r, packet_t *pkt){
  struct packet *ackpack = (struct packet*)malloc(sizeof(struct packet));
  makeAckPacket(r, ackpack, ntohl(pkt->seqno));
  conn_sendpkt(r->c, ackpack, ntohs(ackpack->len));
  if(d==1)fprintf(stderr, "id %d sent ack %d\n", r->id, ntohl(ackpack->ackno));

 // free(p);
}

//send a data packet
void sendPacket(rel_t *r, packet_t *pkt, int seqNo){

  struct packet_info *pinfo = (struct packet_info*)malloc(sizeof(struct packet_info));

  pinfo->r = r;
  pinfo->size=ntohs(pkt->len);
  pinfo->ackNo = r->nextAckNum;
  pinfo->seqNo = seqNo;
  pinfo->p = pkt;

  pthread_t tid;
  pthread_create(&tid, NULL, timer,(void *) pinfo);


  conn_sendpkt(r->c, pkt, ntohs(pkt->len));
  if(d==1)
    fprintf(stderr,"Packet sent from %d with size %d and seqno %d\n",r->id,ntohs(pkt->len), seqNo);  

}


void *timer(void *vargp){
  struct packet_info *pinfo = (struct packet_info*) vargp;
  int prevAckNum = pinfo->ackNo;
  int size = pinfo->size;
  rel_t *r = pinfo->r;
  int seqNo = pinfo->seqNo;
  struct packet *pkt = pinfo->p; 
  // free(pinfo->r);
  

  sleep(r->timeout/1000);
  if(prevAckNum == r->nextAckNum){
    if(d==1)fprintf(stderr, "need to retransmit! %d\n", seqNo);
    sendPacket(r,pkt, seqNo);
  }
  //free(pkt);
  return NULL;
}