#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <unistd.h>

#include "replication.h"
#include "istgt_integration.h"
#include "istgt_scsi.h"
#include "istgt_misc.h"
#include "replication_misc.h"

typedef struct cargs_s {
	spec_t *spec;
	int workerid;
} cargs_t;


void create_mock_client(spec_t *);
void *reader(void *args);
void *writer(void *args);
void check_settings(spec_t *spec);
void build_cmd(cargs_t *cargs, ISTGT_LU_CMD_Ptr lu_cmd, int opcode,
    int len);

void
build_cmd(cargs_t *cargs, ISTGT_LU_CMD_Ptr cmd, int opcode,
    int len)
{
	char *buf;
	int i;

	cmd->luworkerindx = cargs->workerid;
	if (opcode == 0) {
		buf = xmalloc(len);
		for (i = 0; i < len; i++)
			buf[i] = random() % 200;
		cmd->cdb0 = SBC_WRITE_16;
		cmd->iobuf[0].iov_base = buf;
		cmd->iobuf[0].iov_len = len;
		cmd->iobufsize = len;
	} else {
		cmd->cdb0 = SBC_READ_16;
		cmd->iobufsize = len;
	}
}

void check_settings(spec_t *spec)
{
	while (spec->ready != true)
		sleep(1);

	return;
}

void *
writer(void *args)
{
	cargs_t *cargs = (cargs_t *)args;
	spec_t *spec = (spec_t *)cargs->spec;
	int blkcnt = spec->blockcnt;
	int blklen = spec->lu->blocklen;
	int num_blocks = (blkcnt - 16);
	ISTGT_LU_CMD_Ptr lu_cmd;
	int rc, count = 0;
	char tname[50];
	uint64_t blk_offset, offset;
	int len_in_blocks, len;
	struct timespec now, start, prev;

	snprintf(tname, 50, "mcwrite%d", cargs->workerid);
	prctl(PR_SET_NAME, tname, 0, 0, 0);
	clock_gettime(CLOCK_MONOTONIC, &start);
	clock_gettime(CLOCK_MONOTONIC, &prev);

	lu_cmd  = malloc(sizeof (ISTGT_LU_CMD));
	memset(lu_cmd, 0, sizeof (ISTGT_LU_CMD));

	while (1) {
		check_settings(spec);

		blk_offset = random() % num_blocks;
		offset = blk_offset * blklen;
		len_in_blocks = random() & 15;
		len = len_in_blocks * blklen;

		build_cmd(cargs, lu_cmd, 0, len);

		rc = replicate(spec, lu_cmd, offset, len);
		if (rc != len)
			goto end;
		count++;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec > 10)
			break;
		if (now.tv_sec - prev.tv_sec > 1) {
			prev = now;
			printf("wrote %d from %s\n", count, tname);
		}
	}
end:
	printf("wrote %d from %s\n", count, tname);
	return NULL;
}

void *
reader(void *args)
{
	cargs_t *cargs = (cargs_t *)args;
	spec_t *spec = (spec_t *)cargs->spec;
	int blkcnt = spec->blockcnt;
	int blklen = spec->lu->blocklen;
	int num_blocks = (blkcnt - 16);
	ISTGT_LU_CMD_Ptr lu_cmd;
	int rc, count = 0;
	char tname[50];
	uint64_t blk_offset, offset;
	int len_in_blocks, len;
	struct timespec now, start, prev;

	snprintf(tname, 50, "mcread%d", cargs->workerid);
	prctl(PR_SET_NAME, tname, 0, 0, 0);

	clock_gettime(CLOCK_MONOTONIC, &start);
	clock_gettime(CLOCK_MONOTONIC, &prev);

	lu_cmd  = malloc(sizeof (ISTGT_LU_CMD));
	memset(lu_cmd, 0, sizeof (ISTGT_LU_CMD));

	while (1) {
		check_settings(spec);

		blk_offset = random() % num_blocks;
		offset = blk_offset * blklen;
		len_in_blocks = random() & 15;
		len = len_in_blocks * blklen;

		build_cmd(cargs, lu_cmd, 1, len);

		rc = replicate(spec, lu_cmd, offset, len);
		if (rc != len)
			goto end;

		free(lu_cmd->data);
		lu_cmd->data = NULL;

		count++;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec > 10)
			break;
		if (now.tv_sec - prev.tv_sec > 1) {
			prev = now;
			printf("read %d from %s\n", count, tname);
		}
	}
end:
	printf("read %d from %s\n", count, tname);
	return NULL;
}

cargs_t *all_cargs;
pthread_t *all_cthreads;

void
create_mock_client(spec_t *spec)
{
	int num_threads = 6;
	int i;
	cargs_t *cargs;
	struct timespec now;

	prctl(PR_SET_NAME, "mockclient", 0, 0, 0);

	clock_gettime(CLOCK_MONOTONIC, &now);
	srandom(now.tv_sec);

	all_cargs = (cargs_t *)malloc(sizeof (cargs_t) * num_threads);
	all_cthreads = (pthread_t *)malloc(sizeof (pthread_t) * num_threads);

	for (i = 0; i < num_threads; i++) {
		cargs = &(all_cargs[i]);
		cargs->workerid = i;
		cargs->spec = spec;
		if (i < num_threads / 2)
			pthread_create(&all_cthreads[i], NULL, &writer, cargs);
		else
			pthread_create(&all_cthreads[i], NULL, &reader, cargs);
	}
//	while (1) {
//		sleep(1);
//	}
}

