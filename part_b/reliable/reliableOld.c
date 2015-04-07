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
#include <math.h>
#include "rlib.h"
#define MAX_DATA_SIZE 1000
#define d 1
#define ACK_PACKET_SIZE 12
#define PACKET_HEADER 16
#define initWindow 1

struct timeout_node{
  struct timeout_node* next;
  packet_t* pkt;
  struct timeval lastTransmission;
};
struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;
  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */
  //for sending side
  int cwnd;
  int rwnd;

  int send_receive;
  int lastByteAcked;
  int lastByteSent;
  int lastByteWritten;
  uint32_t seqNum; //the sequence number you should send starting at 1
  uint32_t nextAckNum; //the next ack number that you expect
  
  int EOFSent;
	int recEOF;
	int readEOF;
  struct timeout_node* timeList;
  //for receiving side
  int lastByteRead;
  int nextByteExpected;
  int lastByteReceived;
  //char *receivingWindow;
  uint32_t nextSeqNum; //the next sequence number the receiver expects

  int sendWindowSize;

  struct pkt_node* head;
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
struct pkt_node{
  packet_t* packet;
  int seqNo;
  struct pkt_node* next;
  struct pkt_node* prev;
};

int readEOF;
int recEOF;


//declared helper functions
//int getSendBufferSize(rel_t *r);
packet_t* makePacket(rel_t *s, int seqno);
//void allignSendingWindow(rel_t *r);
//void allignReceivingWindow(rel_t *r);
void* sendEOF(rel_t* r);
void* sendFinalEOF(void* r);
void updateWindow(rel_t* r, packet_t* recv);
struct ack_packet* makeAndSendAckPacket(rel_t *s, int seqno);
//int getReceiveBufferSize(rel_t *r);
void rel_output2(rel_t *r, packet_t *p, int lenToPrint);
//void sendAck(rel_t *r, packet_t *p);
void* destroy(void* data);
int checkDestroy(rel_t *r);
void *timer(void *vargp);
void sendPacket(rel_t *s, packet_t *pkt, int seqNo);
void initializeBuffer(struct pkt_node* head);
void checkForTimeouts(rel_t* r);
void flushBuffer(rel_t* r, struct pkt_node* head);
void addToBuffer(struct pkt_node* head, packet_t* p);
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
   if(d==1) fprintf(stderr, "nooooo\n");
   c = conn_create (r, ss);
   if (!c) {
    free (r);
    return NULL;
  }
  
}
fprintf(stderr,"created timeouts! \n");
r -> timeList = (struct timeout_node*)malloc(sizeof(struct timeout_node));
r->c = c;
r->next = rel_list;
r->prev = &rel_list;
if (rel_list)
  rel_list->prev = &r->next;
rel_list = r;

r -> send_receive = cc -> sender_receiver;
  r->rwnd=(cc->window /** MAX_DATA_SIZE*/);
  r->sendWindowSize = initWindow /** MAX_DATA_SIZE*/;
  //r->sendingWindow=(char*)malloc(r->sendWindowSize * sizeof(char));
  //r->receivingWindow=(char*)malloc(r->sendWindowSize * sizeof(char));
r -> cwnd = initWindow;
r->nextByteExpected=1;
r->seqNum=1; r->nextAckNum=2; r->nextSeqNum=1;
srand(time(NULL));
r->id = rand();
r->timeout = cc->timeout;
fprintf(stderr,"timeout cc: %f \n", cc -> timeout);
  //sendAck(r);
r -> head = (struct pkt_node*)malloc(sizeof(struct pkt_node));
initializeBuffer(r->head);
if(d==1)fprintf(stderr, "send_receive: %d", r -> send_receive);
if(r-> send_receive == RECEIVER){
  sendEOF(r);
  readEOF = 1;
  if(d==1)fprintf(stderr,"Receiver sent EOF : %d! \n", r -> rwnd);
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
  if(d==1)fprintf(stderr, "connection destroyed!\n");

  //free(r->sendingWindow);
  //free(r->receivingWindow);
  free(r);

}

void
rel_demux (const struct config_common *cc,
  const struct sockaddr_storage *ss,
  packet_t *pkt, size_t len)
{
  if(d==1)fprintf(stderr, "noooo\n");
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

  if(n<(size_t)ntohs(pkt->len)){
    if(d==1)fprintf(stderr,"length check failed!\n");
    return;
  }

  //ack packet
  if(n==ACK_PACKET_SIZE){

    if(d==1)fprintf(stderr,"id %d received ack packet %d length %d\n", r->id,ntohl(pkt->ackno), ntohs(pkt->len));
    

    //an ack greater than or equal to the next expected ack number has arrived!
    //fprintf(stderr, "pack ackno: %d, nextAckNum: %d",ntohl(pkt->ackno), r->nextAckNum );
    if(ntohl(pkt->ackno) == r->nextAckNum){
      // int j;
      //this loop is so that we can move the window even if the ack will cause multiple sent packets to be acked
      // for(j=0; j<ntohl(pkt->ackno) - r->nextAckNum+1; j++){
        // fprintf(stderr,"window aligned\n");
       // r->lastByteAcked+=MAX_DATA_SIZE;
        //allignSendingWindow(r);

      // }
      if(r->send_receive==SENDER){
        updateWindow(r, pkt);
      }
        r->nextAckNum=ntohl(pkt->ackno)+1;
        r->packetsInFlight-=1;

      //now we are waiting for the next ack

        rel_read(r);
        checkDestroy(r);



    }

      else{
        if(d==1)fprintf(stderr,"weird ack received\n");
      }
    }
  //dataPacket
    else{
      if(d==1)fprintf(stderr,"id %d received data packet of length %d seq %d\n", r->id,n, ntohl(pkt->seqno));

      if(ntohl(pkt->ackno) == r->nextAckNum){
        if(d==1)fprintf(stderr, "ahhh reallllllllllllllllllllllllllllll\n");
      }
  //if we've already acked this packet, our ack was lost so ack it again!
      if(ntohl(pkt->seqno)<r->nextSeqNum){
        if(d==1)fprintf(stderr,"reacking cause ack was lost\n");
        makeAndSendAckPacket(r, ntohl(pkt->seqno)+1);
        return;
      }
    //if we've received an EOF, check if we should destroy the connection

      if(recEOF != 1 && n == PACKET_HEADER){
        if(d==1)fprintf(stderr, "eof received\n");
        recEOF=1;
        conn_output (r->c, NULL, 0);
      //r->nextSeqNum+=1;
        if(r -> send_receive == SENDER){
          updateWindow(r, pkt);
          r-> cwnd --;
          r-> sendWindowSize --;
        }
        makeAndSendAckPacket(r, ntohl(pkt->seqno)+1);
        checkDestroy(r);
        return;
      }
      if(n!=PACKET_HEADER){
    addToBuffer(r->head, pkt);
    flushBuffer(r, r->head);
  }

    // if this is the next expected packet, output and ack
    //  if(ntohl(pkt->seqno)==r->nextSeqNum && n != PACKET_HEADER){
      // rel_output2(r,pkt, n-PACKET_HEADER);
     //}






   }
 }


 void
 rel_read (rel_t *s)
 {
  int isData;
  if(d==1)fprintf(stderr, "called rel_read() as: %d \n", s -> send_receive);
  if(readEOF==1)return NULL;

  // if(s->nextAckNum==s->seqNum)return;
  if(d==1)fprintf(stderr, "reading\n");

  //loop while you still have buffer space to store data
 // fprintf(stderr, "pif: %d \n",s->packetsInFlight );
  if(s->packetsInFlight < s->sendWindowSize/*/MAX_DATA_SIZE*/){
  struct packet *p = makePacket(s, s->seqNum);
  if(p==NULL){
    if(d==1)fprintf(stderr, "nothing to read\n");
    return NULL;
  }

  s->lastByteWritten+=MAX_DATA_SIZE;
  s->lastByteSent+=MAX_DATA_SIZE;
  sendPacket(s, p, s->seqNum);
  s->seqNum+=1;
  s->packetsInFlight+=1;
  if(d==1)fprintf(stderr, "sent Packet\n");
}
else{
  if(d==1)fprintf(stderr, "no room\n");
}
}

void
rel_output (rel_t *r)
{
  if(d==1)fprintf(stderr, "in output\n");
 //rel_output2(r, 500); 

}

void rel_output2(rel_t *r, packet_t *pkt, int lenToPrint){
  //there is no room to write!
  if(conn_bufspace(r->c)<lenToPrint){
  	if(d==1)fprintf(stderr, "not enough room to write\n");
    return;
  }

  int success = conn_output(r->c, pkt->data, lenToPrint);
  //everything was printed, send an ack
  if(success == lenToPrint){
    // fprintf(stderr, "outputted %s\n", pkt->data);
    makeAndSendAckPacket(r, ntohl(pkt->seqno)+1);
    r->nextSeqNum+=1;
    
  }
  else{
    if(d==1)fprintf(stderr, "not outputed\n");
  }
  // rel_read(r);
}

void
rel_timer ()
{
  //fprintf(stderr, "timer called \n");
  //rel_t* r = rel_list;
  //while(r){
    checkForTimeouts(rel_list);
    //r = r -> next;
 // }
  //checkForTimeouts();
  // printf("timer called\n");
  // int i;
  // for(i=0; i<lastByteSent/500+1; i++){

  // }
//fprintf(stderr, "THEO'S TIMEOUT \n");
}


packet_t * makePacket(rel_t *s, int seqno){
  packet_t *p = xmalloc (sizeof (*p));

  int isData = conn_input (s->c, p->data, MAX_DATA_SIZE);
  if(isData==0){
    return NULL;
  }
  if(isData==-1){
    p->len = htons(PACKET_HEADER);
    if(d==1)fprintf(stderr, "readEOF\n");
    readEOF=1;

  }
  else{
    p->len = htons(isData+PACKET_HEADER);
  }


//  p->data[sizeOfData]='\0';
  p->rwnd = htonl(s -> rwnd);
  p->ackno=htonl(1);
  p->seqno=htonl(seqno);
  p->cksum=0;
  p->cksum = cksum(p, ntohs(p->len));
  return p;
  
}

struct ack_packet * makeAndSendAckPacket(rel_t *s, int seqno){
  struct ack_packet *p= xmalloc (sizeof (*p));
  p->len=htons(ACK_PACKET_SIZE);
  p->ackno = htonl(seqno);
  p->rwnd = htonl(s -> rwnd);
  p->cksum=0;
  p->cksum = cksum(p,ntohs(p->len)); 
  conn_sendpkt(s->c, (packet_t*)p, ACK_PACKET_SIZE);
  if(d==1)fprintf(stderr, "id %d sent ack %d\n", s->id, ntohl(p->ackno));
}


//check whether a connection should be destroyed. this is only called if we get an EOF or if we get a -1 on read
int checkDestroy(rel_t *r){
  if(d==1)fprintf(stderr, "checking to destroy %d %d\n", recEOF, readEOF);
  if(readEOF==1 && recEOF==1){
    if(d==1)fprintf(stderr, "first two\n");
    //you have no more packets to ack
    //fprintf(stderr, "lba: %d lbs: %d",r->lastByteAcked, r->lastByteSent );
    if(d==1)fprintf(stderr, "%d\n", r->packetsInFlight );
    if(r->packetsInFlight==0){
      //you have outputed all data
      //fprintf(stderr, "lbread: %d lbreceived: %d",r->lastByteRead, r->lastByteReceived );

      //if(r->lastByteRead==r->lastByteReceived){
      if(d==1)fprintf(stderr,"chose to destroy!\n");
      pthread_t tid;
      pthread_create(&tid, NULL, destroy,(void*)r);
      return 1;
      //}
    }
  }
  return 0;
}
void* destroy(void* data){
  sleep(1);
  rel_destroy((rel_t*)data);
}
/*void sendAck(rel_t *r, packet_t *pkt){
  struct packet *ackpack = (struct packet*)malloc(sizeof(struct packet));
  makeAckPacket(r, ackpack, ntohl(pkt->seqno));
  conn_sendpkt(r->c, ackpack, ntohs(ackpack->len));
  if(d==1)fprintf(stderr, "id %d sent ack %d\n", r->id, ntohl(ackpack->ackno));

 // free(p);
}*/

//send a data packet
  void sendPacket(rel_t *r, packet_t *pkt, int seqNo){

    //struct packet_info *pinfo = (struct packet_info*)malloc(sizeof(struct packet_info));

    //pinfo->r = r;
    //pinfo->size=ntohs(pkt->len);
    //pinfo->ackNo = seqNo+1;
    //pinfo->seqNo = seqNo;
    //pinfo->p = pkt;

    //pthread_t tid;
    //pthread_create(&tid, NULL, timer,(void *) pinfo);
    fprintf(stderr, "adding packet totimeout \n");
    addPacketToTimeout(r, pkt);
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


//fprintf(stderr, "timeout %d", r -> timeout);
    sleep(5/10);
    fprintf(stderr, "prevAck: %d expected: %d", prevAckNum, r -> nextAckNum);
    if(prevAckNum == r->nextAckNum){
      fprintf(stderr, "need to retransmit! %d\n", seqNo);
      r -> cwnd = initWindow;
      r -> sendWindowSize = r -> cwnd;
      sendPacket(r,pkt, seqNo);
    }
  //free(pkt);
    return NULL;
  }
  void updateWindow(rel_t* r, packet_t* recv){
    r -> cwnd ++;
    r -> rwnd = ntohl(recv -> rwnd);
  //finds the minimum
    r -> sendWindowSize = (r -> rwnd < r -> cwnd)?r->rwnd:r->cwnd;
    fprintf(stderr, "updated rwnd to: %d updated cwnd to: %d updated currWindow to: %d \n", r -> rwnd, r -> cwnd, r -> sendWindowSize);
  }
  void* sendEOF(rel_t* r){
    packet_t* p = malloc(sizeof(packet_t));
    p -> len = htons(PACKET_HEADER);
    p->rwnd = htonl(r -> rwnd);
    p->ackno=htonl(1);
    p->seqno=htonl(r -> seqNum);
    p->cksum=0;
    p->cksum = cksum(p, ntohs(p->len));
    r -> packetsInFlight++;
    sendPacket(r, p, r -> seqNum);
    if(d==1)fprintf(stderr,"SENT EOF\n");
    return NULL;
  }

  void initializeBuffer(struct pkt_node* head){
   struct pkt_node* tail = (struct pkt_node*) malloc(sizeof(struct pkt_node));
   head -> next = tail;
   head -> seqNo = -1;
   head -> packet = NULL;
   head -> prev = NULL;
   tail -> next = NULL;
   tail -> packet = NULL;
   tail -> prev = head;
   tail -> seqNo = 9999999;
 }
 void addToBuffer(struct pkt_node* head, packet_t* p){
   int seqNo =ntohl(p -> seqno);
   struct pkt_node* toAdd = (struct pkt_node*) malloc(sizeof(struct pkt_node));
   toAdd -> packet = p;
   toAdd -> seqNo = seqNo;
   struct pkt_node* currNode = head;
   if(head -> next -> packet == NULL){
    toAdd -> next = head -> next;
    toAdd -> prev = head;
    head -> next -> prev = toAdd;
    head -> next = toAdd;
  }
//	currNode = currNode -> next;
  while(currNode -> next != NULL){
    if(currNode -> seqNo != seqNo && currNode -> next != seqNo)
      if(currNode -> seqNo < seqNo && currNode -> next -> seqNo > seqNo){
        fprintf(stderr, "packet added to Linked List %d\n", seqNo);
        toAdd -> next = currNode -> next;
        toAdd -> prev = currNode;
        currNode -> next -> prev = toAdd;
        currNode -> next = toAdd;
        break;
      }
      currNode = currNode -> next;
    } 
  }

  void flushBuffer(rel_t* r, struct pkt_node* head){
   int nextExpected = r -> nextSeqNum;
   struct pkt_node* currNode = head -> next;
   if(head -> next -> seqNo == nextExpected){
    while(currNode -> next != NULL){
     if(currNode -> seqNo == nextExpected){
      rel_output2(r, currNode -> packet, ntohs(currNode -> packet -> len)-PACKET_HEADER);
      currNode -> prev -> next = currNode -> next;
      currNode -> next -> prev = currNode -> prev;
      fprintf(stderr, "%d , %d-> ", currNode -> seqNo, nextExpected);
    }
			//NEED TO FREE IT MAYBE
    else{
      fprintf(stderr, "\n");
      return;}
      currNode = currNode -> next;
      nextExpected ++;
    }
  }
  fprintf(stderr, "\n");		
}
void checkForTimeouts(rel_t* r){
  
  struct timeout_node* timeoutHead = r -> timeList;
  struct timeout_node* currNode = timeoutHead;
  struct timeout_node* prevVal = timeoutHead;
  struct timeval currTime;
  //fprintf(stderr, "check for TO \n");
   gettimeofday(&currTime, NULL);
  while(currNode != NULL){
  // fprintf(stderr, "looping! \n");
    prevVal = currNode;
    currNode = currNode -> next;
    if((void*)currNode == NULL){return;}
    int timeDiff = ((int)(currTime.tv_sec) * 1000 + (int)(currTime.tv_usec)/1000) - ((int)(currNode -> lastTransmission.tv_sec)*1000 + (int)(currNode -> lastTransmission.tv_usec)/1000);
	fprintf(stderr, "packet num: %d, nextAckNum: %d \n", r -> nextAckNum, currNode -> pkt -> seqno);
    if(timeDiff >= r -> timeout/* && r->nextAckNum == currNode -> pkt -> seqno*/){
      fprintf(stderr,"found for timeouts! \n");
      prevVal -> next = currNode -> next;
      sendPacket(r, currNode -> pkt, currNode -> pkt -> seqno);

    }
    currNode = currNode -> next;
  }
}
void addPacketToTimeout(rel_t* r, packet_t* packet){
  struct timeout_node* timeoutHead = r -> timeList;
  struct timeout_node* currNode = timeoutHead;
  fprintf(stderr, "adding packet %d to timeout list \n", packet -> seqno);
  if(r -> timeList == NULL) fprintf(stderr, "NO FUCKIN WAY \n");
  //if(currNode -> next != NULL){
  while(currNode -> next != NULL){
    currNode = currNode -> next;
  }
  currNode->next = (struct timeout_node*)malloc(sizeof(struct timeout_node));
  currNode->next -> pkt = packet;
  gettimeofday(&(currNode -> next -> lastTransmission), NULL);
}
