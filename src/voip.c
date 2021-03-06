
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <portaudio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include "markov.h"

#define SAMPLE_RATE (8000)
#define FRAMES_PER_BUFFER (128)
#define PA_SAMPLE_TYPE  paFloat32

const float secs_per_buffer = ((float)FRAMES_PER_BUFFER)/((float)SAMPLE_RATE);

typedef float SAMPLE;
typedef uint8_t QSAMPLE;

float high_lr = 0.2f;
float low_lr = 0.05f;
float low_meantime = 1.0f;
float high_meantime = 0.3f;

int connected = 0;

static int packets_accepted = 1;
static int packets_dropped = 0;

inline QSAMPLE sample_to_qsample(SAMPLE x);
inline SAMPLE qsample_to_sample(QSAMPLE x);

SAMPLE src_filter_num[] = {-1.0f, 1.0f};
SAMPLE src_filter_den[] = {2.0f};
SAMPLE dst_filter_num[] = {0.000266f, 0.000664f, 0.001310f, 0.002325f, 0.003816f, 0.005874f, 0.008558f, 0.011888f, 0.015845f, 0.020359f, 0.025320f, 0.030575f, 0.035937f, 0.041197f, 0.046136f, 0.050535f, 0.054195f, 0.056942f, 0.058646f, -0.940777f, 0.058646f, 0.056942f, 0.054195f, 0.050535f, 0.046136f, 0.041197f, 0.035937f, 0.030575f, 0.025320f, 0.020359f, 0.015845f, 0.011888f, 0.008558f, 0.005874f, 0.003816f, 0.002325f, 0.001310f, 0.000664f, 0.000266f};
SAMPLE dst_filter_den[] = {-2.0f, 2.0f};

float distort(float x);
float undistort(float x);
float sgn(float x);

const float mu = 255.0f;
const float qu = 255.0f;

QSAMPLE sample_to_qsample(SAMPLE x) {
  return (uint8_t)lroundf(qu*((distort(x)+1.0f)/2.0f));
}

SAMPLE qsample_to_sample(QSAMPLE x) {
  return undistort((2.0f*(((float)x)/qu))-1.0f);
}

float sgn(float x)
{
  if (x < 0.0f) return -1.0f;
  else if(x == 0.0f) return 0.0f;
  else return 1.0f;
}

float distort(float x)
{
  return sgn(x)*logf(1.0f+mu*fabsf(x))/logf(1.0f+mu);
}

float undistort(float y)
{
  return sgn(y)*(1.0f/mu)*(powf(1.0f+mu,fabsf(y))-1);
}

static int voipCallback( const void *inputBuffer, void *outputBuffer,
                           unsigned long framesPerBuffer,
                           const PaStreamCallbackTimeInfo* timeInfo,
                           PaStreamCallbackFlags statusFlags,
                           void *userData );

typedef struct {
  SAMPLE* src;
  SAMPLE* dst;
  int num_ord;
  int den_ord;
  const SAMPLE* num;
  const SAMPLE* den;
} FilterData;

void filterdata_init(FilterData* pfd, const SAMPLE* num, int num_ord, const SAMPLE* den, int den_ord)
{
  pfd->src = malloc((FRAMES_PER_BUFFER+num_ord)*sizeof(SAMPLE));
  pfd->dst = malloc((FRAMES_PER_BUFFER+den_ord)*sizeof(SAMPLE));
  pfd->num_ord = num_ord;
  pfd->den_ord = den_ord;
  pfd->num = num;
  pfd->den = den;
  for(int i = 0; i < num_ord; i++) {
    pfd->src[i] = 0.0f;
  }
  for(int i = 0; i < den_ord; i++) {
    pfd->dst[i] = 0.0f;
  }
}

SAMPLE* filter_src(FilterData* pfd);
void filter(FilterData* pfd);
const SAMPLE* filter_dst(FilterData* pfd);

typedef enum CONCEALMODE {
  CM_SILENCE,
  CM_REPEAT,
  CM_NOISE
} CONCEALMODE;

typedef struct {
  //buffering data
  QSAMPLE* inbuf;
  volatile int inbuf_valid;
  QSAMPLE* outbuf;
  volatile int outbuf_valid;
  //filtering data
  FilterData infilter;
  FilterData outfilter;
  //concealment data
  SAMPLE* prev_frame;
  CONCEALMODE conceal_mode;
} VoipData;

void voipdata_init(VoipData* pvd, CONCEALMODE conceal_mode)
{
  pvd->inbuf = malloc(FRAMES_PER_BUFFER*sizeof(QSAMPLE));
  pvd->inbuf_valid = 0;
  pvd->outbuf = malloc(FRAMES_PER_BUFFER*sizeof(QSAMPLE));
  pvd->outbuf_valid = 0;
  filterdata_init(&pvd->infilter, src_filter_num, sizeof(src_filter_num)/sizeof(SAMPLE)-1, src_filter_den, sizeof(src_filter_den)/sizeof(SAMPLE)-1);
  filterdata_init(&pvd->outfilter, dst_filter_num, sizeof(dst_filter_num)/sizeof(SAMPLE)-1, dst_filter_den, sizeof(dst_filter_den)/sizeof(SAMPLE)-1);
  pvd->prev_frame = malloc(FRAMES_PER_BUFFER*sizeof(SAMPLE));
  for(int i = 0; i < FRAMES_PER_BUFFER; i++) pvd->prev_frame[i] = 0.0f;
  pvd->conceal_mode = conceal_mode;
}

static int voipCallback( const void *inputBuffer, void *outputBuffer,
                           unsigned long framesPerBuffer,
                           const PaStreamCallbackTimeInfo* timeInfo,
                           PaStreamCallbackFlags statusFlags,
                           void *userData )
{
  VoipData* data = (VoipData*)userData;
  
  if((inputBuffer != NULL)||(data->inbuf_valid == 0)) {
    //copy the data in for filtering
    memcpy(filter_src(&data->infilter),inputBuffer,FRAMES_PER_BUFFER*sizeof(SAMPLE));
    //do the filtering
    filter(&data->infilter);
    //retrieve the data
    const SAMPLE* rptr = filter_dst(&data->infilter);
    //quantize the data
    for(int i = 0; i < framesPerBuffer; i++) {
      data->inbuf[i] = sample_to_qsample(rptr[i]);
    }
    data->inbuf_valid = 1;
  }
  if(data->outbuf_valid == 1) {
    //unquantize the data and copy it in for filtering
    SAMPLE* fptr = filter_src(&data->outfilter);
    for(int i = 0; i < framesPerBuffer; i++) {
      fptr[i] = qsample_to_sample(data->outbuf[i]);
    }
    //do the filtering
    filter(&data->outfilter);
    //retrieve the data
    memcpy(outputBuffer,filter_dst(&data->outfilter),FRAMES_PER_BUFFER*sizeof(SAMPLE));
    memcpy(data->prev_frame,filter_dst(&data->outfilter),FRAMES_PER_BUFFER*sizeof(SAMPLE));
    data->outbuf_valid = 0;
  }
  else {
    SAMPLE* pout = (SAMPLE*)outputBuffer;
    switch(data->conceal_mode) {
      case CM_SILENCE:
        for(int i = 0; i < framesPerBuffer; i++) {
          pout[i] = 0.0f;
        }
        break;
      case CM_REPEAT:
        for(int i = 0; i < framesPerBuffer; i++) {
          pout[i] = data->prev_frame[i];
        }
        break;
      case CM_NOISE:
        {
          float acc = 0.0f;
          for(int i = 0; i < framesPerBuffer; i++) {
            acc += (data->prev_frame[i]*data->prev_frame[i]);
          }
          acc /= (float)framesPerBuffer;
          acc = sqrt(acc);
          for(int i = 0; i < framesPerBuffer; i++) {
            float rr = randgaussian()*acc;
            if(rr > 1.0f) rr = 1.0f;
            if(rr < -1.0f) rr = -1.0f;
            pout[i] = rr;
          }
        }
        break;
      default:
        fprintf(stderr,"Critical error: Invalid conceal mode (%u).\n",data->conceal_mode);
        exit(1);
    }
  }
  return paContinue;
}

typedef struct {
  int is_server;
  in_addr_t ip_addr;
  uint16_t ip_port;
  CONCEALMODE conceal_mode;
} VoipArgs;

typedef struct {
  int sockfd;
  struct sockaddr_in saddr;
  socklen_t saddrlen;
} ConnectionData;

//returns 0 for success, 1 for failure
int parse_args(VoipArgs* pva, int argc, char* argv[]);

//returns 0 for success, 1 for failure
int connection_init(ConnectionData* pcd, const VoipArgs* pva);
int connection_send(ConnectionData* pcd, const void* buf, size_t length);
int connection_recv(ConnectionData* pcd, void* buf, size_t length);

int main(int argc, char* argv[])
{
  //parse the arguments
  VoipArgs va;
  if(parse_args(&va,argc,argv) != 0) {
    return 1;
  }
  //set up the markov model
  markov_initmodel_fromfile(stdin);
  //markov_initmodel_lowhigh(low_lr,high_lr,low_meantime/secs_per_buffer,high_meantime/secs_per_buffer);
  //initialize the connection  
  ConnectionData cd;
  if(connection_init(&cd,&va) != 0) {
    return 1;
  }
  
  //initialize VOIP data
  VoipData vd;
  voipdata_init(&vd,va.conceal_mode);
  
  fprintf(stderr,"Starting audio stream...\n");
  
  PaStream* stream;
  PaError err;
  
  err = Pa_Initialize();
  if( err != paNoError) goto error;
  
  err = Pa_OpenDefaultStream(
    &stream,
    1, //input channels
    1, //output channels
    PA_SAMPLE_TYPE,
    SAMPLE_RATE,
    FRAMES_PER_BUFFER,
    voipCallback,
    &vd);
  if(err != paNoError) goto error;
  
  err = Pa_StartStream(stream);
  if(err != paNoError) goto error;
   
  while(1) {
    //wait for input data
    if(vd.inbuf_valid == 1) {
      //send it on the network
      if(connection_send(&cd,vd.inbuf,FRAMES_PER_BUFFER*sizeof(QSAMPLE))!=0) goto error;
      //signal that the input data has been used
      vd.inbuf_valid = 0;
    }
    //wait for output data to be consumed
    if(vd.outbuf_valid == 0) {
      //receive some from the network
      int result;
      int makebufvalid = 0;
      while((result = connection_recv(&cd,vd.outbuf,FRAMES_PER_BUFFER*sizeof(QSAMPLE)))==0) {
        makebufvalid = 1;
      }
      if(result < 0) goto error;
      if(makebufvalid) {
        //signal that the output data is now valid
        vd.outbuf_valid = 1;
      }
    }
  }
  
error:
  if(stream) {
    Pa_AbortStream(stream);
    Pa_CloseStream(stream);
  }
  free(vd.inbuf);
  free(vd.outbuf);
  Pa_Terminate();
  if(shutdown(cd.sockfd,SHUT_RDWR)!=0) {
    fprintf(stderr,"Error: Socket shutdown failed.\n");
  }
  fprintf(stderr,"An error occured while using the portaudio stream\n");
  fprintf(stderr,"Error number: %d\n", err);
  fprintf(stderr,"Error message: %s\n", Pa_GetErrorText(err));
  return -1;
}


int connection_send(ConnectionData* pcd, const void* buf, size_t length)
{
  int result = sendto(pcd->sockfd, buf, length, 0, (struct sockaddr*) &pcd->saddr, pcd->saddrlen);
  if(result < 0) {
    fprintf(stderr,"Error: Send call failed.\n");
    return 1;
  }
  return 0;
}

//This function is modified to model packet loss
int connection_recv(ConnectionData* pcd, void* buf, size_t length)
{
  int msglen = recvfrom(pcd->sockfd, buf, length, 0, (struct sockaddr*)&pcd->saddr, &pcd->saddrlen);
  if(msglen <= 0) {
    if((errno == EAGAIN)||(errno == EWOULDBLOCK)) {
      return 1;
    }
    else {
      fprintf(stderr,"Error: Recieve call failed.\n");
      return -1;
    }
  }
  else {
    if(msglen != length) {
      fprintf(stderr,"Error: Recieved message has expected length.\n");
      return -1;
    }
  }
  if((packets_dropped + packets_accepted)%100 == 0) {
    fprintf(stderr,"Drop rate: %f\n",(float)packets_dropped/(float)(packets_dropped+packets_accepted));
  }
  //transition states as needed
  markov_transition();
  //based on state, call emit function which will decide to drop or accept packet based on prob
  mmvalue result = markov_emission();
  if(connected && (result == -1 || result == MM_DROP)) {
    packets_dropped++;
    return 1;
  }
  //returning 0 on success
  packets_accepted++;
  return 0;
}

int connection_init(ConnectionData* pcd, const VoipArgs* pva)
{
  uint8_t msgbuf[256]; int result;
  
  const uint8_t client_message[] = {57, 36, 104, 39, 11, 207, 192, 126}; 
  const uint8_t server_message[] = {23, 24, 62, 111, 204, 123, 3, 2};
  
  pcd->saddrlen = sizeof(pcd->saddr); 
  bzero(&pcd->saddr, sizeof(pcd->saddr));
  pcd->saddr.sin_family = AF_INET;
  pcd->saddr.sin_port = htons((uint16_t)pva->ip_port);
  
  pcd->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if(pcd->sockfd < 0) {
    fprintf(stderr,"Error: Couldn't open socket.\n");
    return 1;
  }
  fcntl(pcd->sockfd, F_SETFL, fcntl(pcd->sockfd,F_GETFL,0) | O_NONBLOCK);
  
  if(pva->is_server) {
    //set up and bind the socket
    pcd->saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(pcd->sockfd,(struct sockaddr*)&pcd->saddr,sizeof(pcd->saddr))<0) {
      fprintf(stderr,"Error: Couldn't bind socket.\n");
      return 1;
    }
    //read from the socket (wait for a "connection")
    fprintf(stderr,"Waiting for a connection on port %u...\n",pva->ip_port);
    while((result = connection_recv(pcd,msgbuf,sizeof(client_message)))==1);
    if(result < 0) {
      return 1;
    }
    for(int i = 0; i < sizeof(client_message); i++) {
      if(msgbuf[i] != client_message[i]) {
        fprintf(stderr,"Error: Connection message has unexpected contents.\n");
        return 1;
      }
    }
    //respond with the server message
    fprintf(stderr,"Client found!  Responding...\n");
    if(connection_send(pcd,server_message,sizeof(server_message))!=0) {
      return 1;
    }
    //ready to start audio stream
    connected = 1;
  }
  else {
    pcd->saddr.sin_addr.s_addr = pva->ip_addr;
    //send client message
    fprintf(stderr,"Sending connection message on port %u...\n",pva->ip_port);
    if(connection_send(pcd,client_message,sizeof(client_message))!=0) {
      return 1;
    }
    //receive server message
    fprintf(stderr,"Waiting for server response...\n");
    while((result = connection_recv(pcd,msgbuf,sizeof(server_message)))==1);
    if(result < 0) {
      return 1;
    }
    for(int i = 0; i < sizeof(server_message); i++) {
      if(msgbuf[i] != server_message[i]) {
        fprintf(stderr,"Error: Connection message has unexpected contents.\n");
        return 1;
      }
    }
    fprintf(stderr,"Connected!\n");
    //ready to start audio stream
    connected = 1;
  }
  return 0;
}
/*
typedef struct {
  SAMPLE* src;
  SAMPLE* dst;
  int num_ord;
  int den_ord;
  const SAMPLE* num;
  const SAMPLE* den;
} FilterData;
*/
SAMPLE* filter_src(FilterData* pfd)
{
  return pfd->src + pfd->num_ord;
}

void filter(FilterData* pfd)
{
  /*
  fprintf(stderr, "Notice: Filtering with #num = %d, #den = %d, #FB = %d, #DD = %f\n", pfd->num_ord, pfd->den_ord, FRAMES_PER_BUFFER, pfd->den[pfd->den_ord]);
  fprintf(stderr, "Num =");
  for(int nn = 0; nn <= pfd->num_ord; nn++) fprintf(stderr, " %f", pfd->num[nn]);
  fprintf(stderr, "\nDen =");
  for(int nn = 0; nn <= pfd->den_ord; nn++) fprintf(stderr, " %f", pfd->den[nn]);
  fprintf(stderr, "\n");
  */
  //do the filtering
  for(int i = 0; i < FRAMES_PER_BUFFER; i++) {
    SAMPLE acc = 0.0f;
    for(int k = 0; k <= pfd->num_ord; k++) {
      //if((pfd->src[i+k] >= 1.0f)||(pfd->src[i+k] <= -1.0f)) {
      //  fprintf(stderr, "Warning: Out-of-bounds source value %f\n", pfd->src[i+k]);
      //}
      acc += pfd->num[k]*pfd->src[i+k];
    }
    for(int k = 0; k < pfd->den_ord; k++) {
      //if((pfd->dst[i+k] >= 1.0f)||(pfd->dst[i+k] <= -1.0f)) {
      //  fprintf(stderr, "Warning: Out-of-bounds dst value %f\n", pfd->src[i+k]);
      //}
      acc -= pfd->den[k]*pfd->dst[i+k];
    }
    float result = acc / pfd->den[pfd->den_ord];
    //if((result >= 1.0f)||(result <= -1.0f)) {
    //  fprintf(stderr, "Warning: Out-of-bounds filtered result value %f\n", result);
    //  if(result >= 1.0f) result = 1.0f;
    //  if(result <= -1.0f) result = -1.0f;
    //}
    pfd->dst[pfd->den_ord + i] = result;
  }
  //copy the data down for the next iteration
  for(int k = 0; k < pfd->num_ord; k++) {
    pfd->src[k] = pfd->src[FRAMES_PER_BUFFER+k];
  }
  for(int k = 0; k < pfd->den_ord; k++) {
    pfd->dst[k] = pfd->dst[FRAMES_PER_BUFFER+k];
  }
}

const SAMPLE* filter_dst(FilterData* pfd)
{
  return pfd->dst + pfd->den_ord;
}


int parse_args(VoipArgs* pva, int argc, char* argv[])
{
  if(argc != 4) {
    fprintf(stderr,"Usage: %s [server|ip-address] [port] [conceal mode]\n",argv[0]);
    fprintf(stderr,"  where [conceal mode] is one of:\n");
    fprintf(stderr,"    silence\n");
    fprintf(stderr,"    repeat\n");
    fprintf(stderr,"    noise\n\n");
    return 1;
  }
  
  in_addr_t ip_addr; int is_server;
  if(strcmp(argv[1],"server")==0) {
    is_server = 1;
    ip_addr = (in_addr_t)0;
  }
  else {
    is_server = 0;
    ip_addr = inet_addr(argv[1]);
    if(ip_addr == (in_addr_t)(-1)) {
      fprintf(stderr,"Error: Invalid IP address \"%s\"\n",argv[1]);
      return 1;
    }
  }
    
  int iport = atoi(argv[2]);
  if((iport <= 0)||(iport >= 65536)) {
    fprintf(stderr,"Error: Invalid port \"%s\"\n",argv[2]);
    return 1;
  }
  
  pva->is_server = is_server;
  pva->ip_addr = ip_addr;
  pva->ip_port = (uint16_t)iport;
  
  if(strcmp(argv[3],"silence")==0) {
    pva->conceal_mode = CM_SILENCE;
  }
  else if(strcmp(argv[3],"repeat")==0) {
    pva->conceal_mode = CM_REPEAT;
  }
  else if(strcmp(argv[3],"noise")==0) {
    pva->conceal_mode = CM_NOISE;
  }
  else {
    fprintf(stderr,"Error: Invalid conceal mode \"%s\"\n", argv[3]);
    return 1;
  }
  return 0;
}
