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
#define MAX_DATA_SIZE 500
#define d 1
#define ACK_PACKET_SIZE 8
#define PACKET_HEADER 12
#define initWindow 1

struct timeout_node{
  struct timeout_node* next;
  packet_t* pkt;
  struct timeval lastTransmission;
};

struct reliable_state {
  rel_t *next;      /* Linked list for traversing all connections */
	rel_t **prev;
  conn_t *c;      /* This is the connection object */

  /* Add your own data fields below this */
  //for sending side


  uint32_t seqNum; //the sequence number you should send starting at 1
  uint32_t nextAckNum; //the next ack number that you expect
  
  int recEOF;
  int readEOF;

  struct timeout_node* timeList;
  //for receiving side
  //char *receivingWindow;
  uint32_t nextSeqNum; //the next sequence number the receiver expects

  int sendWindowSize;

  int id;
  int timeout;

  int packetsInFlight;
};
rel_t *rel_list;

packet_t* makePacket(rel_t *s, int seqno);
struct ack_packet* makeAndSendAckPacket(rel_t *s, int seqno);
void rel_output2(rel_t *r, packet_t *p, int lenToPrint);
void rel_destroy(rel_t *r);
void checkDestroy(rel_t *r);
void sendPacket(rel_t *s, packet_t *pkt, int seqNo);
void checkForTimeouts(rel_t* r);
void addPacketToTimeout(rel_t* r, packet_t* packet);


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
// r -> timeList = (struct timeout_node*)malloc(sizeof(struct timeout_node));
	r -> timeList = NULL;
	r->c = c;
	r->next = rel_list;
	r->prev = &rel_list;
	if (rel_list)
		rel_list->prev = &r->next;
	rel_list = r;
	r->sendWindowSize = cc->window;

	r->seqNum=1; r->nextAckNum=2; r->nextSeqNum=1;
	srand(time(NULL));
	r->id = rand();
	r->timeout = cc->timeout;
	return r;
}

void
rel_read (rel_t *s)
{
	if(s->readEOF==1)return;

	if(s->packetsInFlight < s->sendWindowSize){
		struct packet *p = makePacket(s, s->seqNum);
		if(p==NULL){
			if(d==1)fprintf(stderr, "nothing to read\n");
			return;
		}

		sendPacket(s, p, s->seqNum);
		s->seqNum+=1;
		s->packetsInFlight+=1;
	}

}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
      // fprintf(stderr, "rcvd someting %d %d \n", ntohl(pkt->seqno), n);

  if(n!=(size_t)ntohs(pkt->len)){
    if(d==1)fprintf(stderr,"length check failed! %d size %d\n",ntohl(pkt->seqno), n   );
    return;
  }

	uint16_t tempsum = pkt->cksum;

	pkt->cksum=0;
	if(cksum(pkt,ntohs(pkt->len)) != tempsum){      
		if(d==1)fprintf(stderr,"checksum failed packet %d size %d!\n",ntohl(pkt->seqno), n);
		return;
	}

	

  //ack packet
	if(n==ACK_PACKET_SIZE){
		if(d==1)fprintf(stderr,"id %d received ack packet %d length %d\n", r->id,ntohl(pkt->ackno), ntohs(pkt->len));

		if(ntohl(pkt->ackno) == r->nextAckNum){

			r->nextAckNum=ntohl(pkt->ackno)+1;
			r->packetsInFlight-=1;
			checkDestroy(r);
			rel_read(r);
		}

	}
  //dataPacket
	else{
		if(d==1)fprintf(stderr,"id %d received data packet of length %d seq %d\n", r->id,n, ntohl(pkt->seqno));


  //if we've already acked this packet, our ack was lost so ack it again!
		if(ntohl(pkt->seqno)<r->nextSeqNum){
			if(d==1)fprintf(stderr,"reacking cause ack was lost\n");
			makeAndSendAckPacket(r, ntohl(pkt->seqno)+1);
			return;
		}
    //if we've received an EOF, check if we should destroy the connection

		if(r->recEOF != 1 && n == PACKET_HEADER){
			if(d==1)fprintf(stderr, "eof received\n");
			r->recEOF=1;
			conn_output (r->c, NULL, 0);
			makeAndSendAckPacket(r, ntohl(pkt->seqno)+1);
			checkDestroy(r);
			return;
		}
		if(n!=PACKET_HEADER && r->nextSeqNum==ntohl(pkt->seqno)){
			rel_output2(r, pkt, n-PACKET_HEADER);
		}
    else{
      fprintf(stderr, "packet dropped\n");
    }
	}
}

void rel_output2(rel_t *r, packet_t *pkt, int lenToPrint){
	if(conn_bufspace(r->c)<lenToPrint){
		if(d==1)fprintf(stderr, "not enough room to write\n");
		return;
	}

	int success = conn_output(r->c, pkt->data, lenToPrint);
	if(success == lenToPrint){
		makeAndSendAckPacket(r, ntohl(pkt->seqno)+1);
		r->nextSeqNum+=1;
	}
	else{
		if(d==1)fprintf(stderr, "not outputed\n");
	}
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
		s->readEOF=1;
	}
	else{
		p->len = htons(isData+PACKET_HEADER);
	}
	p->ackno=htonl(1);
	p->seqno=htonl(seqno);
	p->cksum=0;
	p->cksum = cksum(p, ntohs(p->len));
	return p;

}

void sendPacket(rel_t *r, packet_t *pkt, int seqNo){

	addPacketToTimeout(r, pkt);
	conn_sendpkt(r->c, pkt, ntohs(pkt->len));
	if(d==1)
		fprintf(stderr,"Packet sent from %d with size %d and seqno %d\n",r->id,ntohs(pkt->len), seqNo);  
}

struct ack_packet * makeAndSendAckPacket(rel_t *s, int seqno){
  struct ack_packet *p= xmalloc (sizeof (*p));
  p->len=htons(ACK_PACKET_SIZE);
  p->ackno = htonl(seqno);
  p->cksum=0;
  p->cksum = cksum(p,ntohs(p->len)); 
  conn_sendpkt(s->c, (packet_t*)p, ACK_PACKET_SIZE);
  if(d==1)fprintf(stderr, "id %d sent ack %d\n", s->id, ntohl(p->ackno));
}

void checkDestroy(rel_t *r){
  if(d==1)fprintf(stderr, "checking to destroy %d %d\n", r->recEOF, r->readEOF);
  if(r->readEOF==1 && r->recEOF==1){
    if(d==1)fprintf(stderr, "first two\n");
    if(d==1)fprintf(stderr, "%d\n", r->packetsInFlight );
    if(r->packetsInFlight==0){
      if(d==1)fprintf(stderr,"chose to destroy!\n");
      rel_destroy(r);
    }
  }
}
void rel_destroy(rel_t *r){
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);
  free (r);
}

void
rel_timer ()
{

  rel_t* r = rel_list;
  while(r){
    checkForTimeouts(r);
    r = r -> next;
  }
}

void addPacketToTimeout(rel_t* r, packet_t* packet){
  struct timeout_node* currNode =  r->timeList;

  if(currNode ==NULL){
    r->timeList = (struct timeout_node*)malloc(sizeof(struct timeout_node));

    r->timeList -> next=NULL;
    r->timeList -> pkt = packet;
    gettimeofday(&(r->timeList -> lastTransmission), NULL);

    return;
  }  
  while(currNode->next != NULL){
    currNode = currNode -> next;
  }
  struct timeout_node* newNode = (struct timeout_node*)malloc(sizeof(struct timeout_node));
  newNode->pkt = packet;
  gettimeofday(&(newNode -> lastTransmission), NULL);//}
  newNode->next = NULL;


  currNode->next = newNode;
  fprintf(stderr, "new node added seqno %d",ntohl(newNode->pkt->seqno) );


}

void checkForTimeouts(rel_t* r){
  struct timeout_node* currNode = r -> timeList;
  struct timeout_node* prevVal = NULL;
  struct timeval currTime;
  gettimeofday(&currTime, NULL);

  fprintf(stderr, "biiiiitch\n");

  while(currNode != NULL){

    //check if packet was acked
    fprintf(stderr, "check if pack was acked next ack exp: %d this packets num+1 %d\n",r->nextAckNum ,ntohl(currNode->pkt->seqno)+1 );
    if(r->nextAckNum > ntohl(currNode->pkt->seqno)+1){
      if(prevVal == NULL){
        r->timeList = currNode->next;
      	currNode = r->timeList;
        continue;
      	
      }
      prevVal->next = currNode->next;

      currNode = currNode ->next;

      continue;
    }
    int timeDiff = ((int)(currTime.tv_sec) * 1000 + (int)(currTime.tv_usec)/1000) - ((int)(currNode -> lastTransmission.tv_sec)*1000 + (int)(currNode -> lastTransmission.tv_usec)/1000);
    fprintf(stderr, "time difffffff: %d and timout: %d\n", timeDiff, r->timeout);
    if(timeDiff >= r -> timeout){
      fprintf(stderr,"found for timeouts! \n");
      if(prevVal == NULL){
        r->timeList = currNode->next;
      }
      else{prevVal -> next = currNode -> next;}
      sendPacket(r, currNode -> pkt, ntohl(currNode -> pkt -> seqno));
      currNode = currNode ->next;
    }
    else{
        prevVal = currNode;
        currNode = currNode -> next;

    }
  }
}

void
rel_demux (const struct config_common *cc,
  const struct sockaddr_storage *ss,
  packet_t *pkt, size_t len)
{
  if(d==1)fprintf(stderr, "noooo\n");
}

void
rel_output (rel_t *r)
{
  if(d==1)fprintf(stderr, "in output\n");
 //rel_output2(r, 500); 

}