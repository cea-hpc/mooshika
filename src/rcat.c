
#include "config.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <unistd.h>	//read
#include <getopt.h>
#include <errno.h>

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "trans_rdma.h"

#define CHUNK_SIZE 512
#define RECV_NUM 3

#define TEST_Z(x)  do { if ( (x)) ERROR_LOG("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_NZ(x) do { if (!(x)) ERROR_LOG("error: " #x " failed (returned zero/null)."); } while (0)

struct datamr {
	void *data;
	struct ibv_mr *mr;
	libercat_data_t *ackdata;
	pthread_mutex_t *lock;
	pthread_cond_t *cond;
};

void callback_send(libercat_trans_t *trans, void *arg) {

}

void callback_recv(libercat_trans_t *trans, void *arg) {
	struct datamr *datamr = arg;
	libercat_data_t **pdata = datamr->data;

	if ((*pdata)->size != 1) {
		write(1, (char *)(*pdata)->data, (*pdata)->size);
		fflush(stdout);

		libercat_post_recv(trans, pdata, datamr->mr, callback_recv, datamr);
		libercat_post_send(trans, datamr->ackdata, datamr->mr, NULL, NULL);
	} else {
		libercat_post_recv(trans, pdata, datamr->mr, callback_recv, datamr);

		pthread_mutex_lock(datamr->lock);
		pthread_cond_signal(datamr->cond);
		pthread_mutex_unlock(datamr->lock);		
	}
}

void print_help(char **argv) {
	printf("Usage: %s {-s|-c addr}\n", argv[0]);
}

int main(int argc, char **argv) {


	libercat_trans_t *trans;
	uint8_t *rdmabuf;
	struct ibv_mr *mr;

	libercat_data_t *wdata;

	libercat_trans_attr_t attr;

	memset(&attr, 0, sizeof(libercat_trans_attr_t));

	attr.server = -1; // put an incorrect value to check if we're either client or server
	// sane values for optional or non-configurable elements
	attr.rq_depth = RECV_NUM+2;
	((struct sockaddr_in*) &attr.addr)->sin_family = AF_INET;
	((struct sockaddr_in*) &attr.addr)->sin_port = htons(1235);

	// argument handling
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	required_argument,	0,		's' },
		{ "port",	required_argument,	0,		'p' },
		{ "help",	no_argument,		0,		'h' },
		{ 0,		0,			0,		 0  }
	};

	int option_index = 0;
	int op;
	while ((op = getopt_long(argc, argv, "@hvsc:p:", long_options, &option_index)) != -1) {
		switch(op) {
			case '@':
				printf("%s compiled on %s at %s\n", argv[0], __DATE__, __TIME__);
				printf("Release = %s\n", VERSION);
				printf("Release comment = %s\n", VERSION_COMMENT);
				printf("Git HEAD = %s\n", _GIT_HEAD_COMMIT ) ;
				printf("Git Describe = %s\n", _GIT_DESCRIBE ) ;
				exit(0);
			case 'h':
				print_help(argv);
				exit(0);
			case 'v':
				ERROR_LOG("verbose switch not ready just yet, come back later!\n");
				break;
			case 'c':
				attr.server = 0;
				inet_pton(AF_INET, optarg, &((struct sockaddr_in*) &attr.addr)->sin_addr);
				break;
			case 's':
				attr.server = 1;
				inet_pton(AF_INET, "0.0.0.0", &((struct sockaddr_in*) &attr.addr)->sin_addr);
				break;
			case 'p':
				((struct sockaddr_in*) &attr.addr)->sin_port = htons(atoi(optarg));
				break;
			default:
				ERROR_LOG("Failed to parse arguments");
				print_help(argv);
				exit(EINVAL);
		}
	}

	if (attr.server == -1) {
		ERROR_LOG("must be either a client or a server!");
		print_help(argv);
		exit(EINVAL);
	}

	TEST_Z(libercat_init(&trans, &attr));

	if (!trans)
		exit(-1);


	if (trans->server) {
		TEST_Z(libercat_bind_server(trans));
		trans = libercat_accept_one(trans);
		//TODO split accept_one in two and post receive requests before the final rdma_accept call
	} else { //client
		TEST_Z(libercat_connect(trans));
	}

	TEST_NZ(rdmabuf = malloc((RECV_NUM+2)*CHUNK_SIZE*sizeof(char)));
	memset(rdmabuf, 0, (RECV_NUM+2)*CHUNK_SIZE*sizeof(char));
	TEST_NZ(mr = libercat_reg_mr(trans, rdmabuf, (RECV_NUM+2)*CHUNK_SIZE*sizeof(char), IBV_ACCESS_LOCAL_WRITE));



	libercat_data_t *ackdata;
	TEST_NZ(ackdata = malloc(sizeof(libercat_data_t)));
	ackdata->data = rdmabuf+(RECV_NUM+1)*CHUNK_SIZE*sizeof(char);
	ackdata->max_size = CHUNK_SIZE*sizeof(char);
	ackdata->size = 1;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&cond, NULL);

	libercat_data_t **rdata;
	struct datamr *datamr;
	int i;

	TEST_NZ(rdata = malloc(RECV_NUM*sizeof(libercat_data_t*)));
	TEST_NZ(datamr = malloc(RECV_NUM*sizeof(struct datamr)));

	for (i=0; i < RECV_NUM; i++) {
		TEST_NZ(rdata[i] = malloc(sizeof(libercat_data_t)));
		rdata[i]->data=rdmabuf+i*CHUNK_SIZE*sizeof(char);
		rdata[i]->max_size=CHUNK_SIZE*sizeof(char);
		datamr[i].data = (void*)&(rdata[i]);
		datamr[i].mr = mr;
		datamr[i].ackdata = ackdata; 
		datamr[i].lock = &lock;
		datamr[i].cond = &cond;
		TEST_Z(libercat_post_recv(trans, &(rdata[i]), mr, callback_recv, &(datamr[i])));
	}
	
	if (trans->server) {
//		TEST_Z(libercat_accept(trans));
	}

	TEST_NZ(wdata = malloc(sizeof(libercat_data_t)));
	wdata->data = rdmabuf+RECV_NUM*CHUNK_SIZE*sizeof(char);
	wdata->max_size = CHUNK_SIZE*sizeof(char);

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(0, &rfds);

	while (1) {

		if (select(1, &rfds, NULL, NULL, NULL) == -1)
			break;
		wdata->size = read(0, (char*)wdata->data, wdata->max_size);
		if (wdata->size == 0)
			break;

		pthread_mutex_lock(&lock);
		TEST_Z(libercat_post_send(trans, wdata, mr, NULL, NULL));
		pthread_cond_wait(&cond, &lock);
		pthread_mutex_unlock(&lock);
	}	


	libercat_destroy_trans(trans);

	return 0;
}