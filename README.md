# 356_congestionControl

sendAck(r);
      
      isEOF=1;
      checkDestroy(r);
      rel_read(r);

1) CHNAGE THE ORDER OF checkDestroy and rel_read because checkDestroy checks (isErr==1 && isEOF==1) and rel_read sets isErr ????

2) ALSO SHOULD IT BE or NOT and ???? (isErr==1 || isEOF==1)
      
3) updating lastByteReceived twice for packets outpf order!!!
		for(i=0; i<(ntohl(pkt->seqno) - r->nextSeqNum); i++){
        r->lastByteReceived+=MAX_DATA_SIZE;
        if(d==1)fprintf(stderr,"space put in receiving window\n");

      }
      //store it at lastbytereceived
      strcpy(r->receivingWindow+ r->lastByteReceived,pkt->data);
      //increment last byte received by a max packet
      r->u+=MAX_DATA_SIZE;


4) 
  sleep(r->timeout/1000);
  if(prevAckNum == r->nextAckNum){
    if(d==1)fprintf(stderr, "need to retransmit! %d\n", seqNo);
    sendPacket(r,size, seqNo);
  }

IF EOF ACK IS LOST WE SO FUUUCCKKKEEDDD BRO
