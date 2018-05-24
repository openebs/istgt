#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/prctl.h>
#include <sys/eventfd.h>
#include "zrepl_prot.h"
#include "replication.h"
#include "istgt_integration.h"
#include "replication_misc.h"
#include "istgt_crc32c.h"
#include "istgt_misc.h"
#include "ring_mempool.h"

cstor_conn_ops_t cstor_ops = {
	.conn_listen = replication_listen,
	.conn_connect = replication_connect,
};

#define	RCMD_MEMPOOL_ENTRIES	100000
rte_smempool_t rcmd_mempool;
size_t rcmd_mempool_count = RCMD_MEMPOOL_ENTRIES;

extern rte_smempool_t rcommon_cmd_mempool;
extern size_t rcommon_cmd_mempool_count;
static void cleanup_replica(replica_t *replica);

#define build_replica_io_hdr() \
{\
	rio->opcode = cmd->opcode;\
	rio->version = REPLICA_VERSION;\
	rio->io_seq = cmd->io_seq;\
	rio->offset = cmd->offset;\
	if (cmd->opcode == ZVOL_OPCODE_WRITE) {\
		rio->len = cmd->data_len + sizeof(struct zvol_io_rw_hdr);\
		rio->checkpointed_io_seq = 0;\
	} else\
		rio->len = cmd->data_len;\
	rio_rw_hdr->io_num = cmd->io_seq;\
	rio_rw_hdr->len = cmd->data_len;\
}

#define build_replica_mgmt_hdr() \
{\
	rmgmtio = (zvol_io_hdr_t *)malloc(sizeof(zvol_io_hdr_t));\
	rmgmtio->opcode = mgmt_opcode;\
	rmgmtio->version = REPLICA_VERSION;\
	rmgmtio->len    = data_len;\
}

/*
#define build_mgmt_cmd() \
{\
	mgmt_cmd = malloc(sizeof(mgmt_cmd_t));\
	mgmt_cmd->opcode = opcode;\
	mgmt_cmd->data_len = malloc \
}
*/

#define build_rcmd() \
{\
	rcmd = get_from_mempool(&rcmd_mempool);\
	rcmd->opcode = cmd->opcode;\
	rcmd->offset = cmd->offset;\
	rcmd->data_len = cmd->data_len;\
	rcmd->io_seq = cmd->io_seq;\
	rcmd->ack_recvd = false;\
	rcmd->rcommq_ptr = cmd;\
	rcmd->status = 0;\
	rcmd->iovcnt = cmd->iovcnt;\
	for (i=1; i < rcmd->iovcnt + 1; i++) {\
		rcmd->iov[i].iov_base = cmd->iov[i].iov_base;\
		rcmd->iov[i].iov_len = cmd->iov[i].iov_len;\
	}\
}

#define check_for_blockage(_cmd, _pending_cmd) {\
	uint64_t current_lba = _cmd->offset; \
	uint64_t current_lbE = current_lba + _cmd->data_len; \
	uint64_t pending_lba = _pending_cmd->offset; \
	uint64_t pending_lbE = pending_lba + _pending_cmd->data_len; \
	if ((current_lbE < pending_lba)  || \
			(pending_lbE < current_lba)) { \
		cmd_blocked = false; \
	} else {\
		cmd_blocked = true; \
	}\
}

#define signal_luworker() { \
	MTX_LOCK(&spec->luworker_rmutex[luworker_id]); \
	pthread_cond_signal(&spec->luworker_rcond[luworker_id]); \
	MTX_UNLOCK(&spec->luworker_rmutex[luworker_id]); \
}

#define	clear_mgmt_cmd(_replica, _mgmt_cmd)		\
	do {						\
		TAILQ_REMOVE(&_replica->mgmt_cmd_queue,	\
			_mgmt_cmd, mgmt_cmd_next);	\
		free(_mgmt_cmd->io_hdr);		\
		free(_mgmt_cmd->data);			\
		free(_mgmt_cmd);			\
	} while(0)

#define	EMPTY_MGMT_Q_OF_REPLICA(_replica)					\
	do {									\
		mgmt_cmd_t *_mgmt_cmd;						\
		while((_mgmt_cmd = TAILQ_FIRST(&replica->mgmt_cmd_queue))) {	\
			clear_mgmt_cmd(_replica, _mgmt_cmd);			\
		}								\
        } while(0)

#define CHECK_FOR_BLOCKAGE_IN_Q(queue, next) {\
	if(cmd_blocked == false) {\
		TAILQ_FOREACH(pending_rcmd, queue, next) {\
			pending_cmd = pending_rcmd->rcommq_ptr;\
			check_for_blockage(cmd, pending_cmd);\
			if(cmd_blocked == true) {\
				TAILQ_INSERT_TAIL(&replica->blockedq, rcmd, rblocked_cmd_next);\
				break;\
			}\
		}\
	}\
}

//Picks cmds from send queue and sends them to replica
void *
replicator(void *arg) {
	spec_t *spec = (spec_t *)arg;
	replica_t *replica;
	bool read_cmd_sent;
	int i = 0;
	rcommon_cmd_t *cmd = NULL;
	rcmd_t *rcmd = NULL;
	bool cmd_blocked = false;
	rcommon_cmd_t *pending_cmd;
	rcmd_t *pending_rcmd;

	while(1) {
		MTX_LOCK(&spec->rcommonq_mtx);
dequeue_common_sendq:
		cmd = TAILQ_FIRST(&spec->rcommon_sendq);
		if(!cmd) {
			pthread_cond_wait(&spec->rcommonq_cond, &spec->rcommonq_mtx);
			goto dequeue_common_sendq;
		}
		cmd->io_seq = ++spec->io_seq;
		cmd->acks_recvd = 0;
		TAILQ_REMOVE(&spec->rcommon_sendq, cmd, send_cmd_next);
		cmd->state = CMD_ENQUEUED_TO_WAITQ;
		TAILQ_INSERT_TAIL(&spec->rcommon_waitq, cmd, wait_cmd_next);
		MTX_UNLOCK(&spec->rcommonq_mtx);

		//Enqueue to individual replica cmd queues and send on the respective fds
		read_cmd_sent = false;
		MTX_LOCK(&spec->rq_mtx);

		TAILQ_FOREACH(replica, &spec->rq, r_next) {
			if(replica == NULL) {
				REPLICA_LOG("Replica not present");
				MTX_UNLOCK(&spec->rq_mtx);
				exit(EXIT_FAILURE);
			}
			//Create an entry for replica queue
			build_rcmd();
			MTX_LOCK(&replica->r_mtx);
			if(replica->removed) {
				MTX_UNLOCK(&replica->r_mtx);
				continue;
			}
			cmd->copies_sent++;

			if (!TAILQ_EMPTY(&replica->blockedq))
				TAILQ_INSERT_TAIL(&replica->blockedq, rcmd, rblocked_cmd_next);
			else {
				cmd_blocked = false;
				CHECK_FOR_BLOCKAGE_IN_Q((&replica->sendq), rsend_cmd_next)
				CHECK_FOR_BLOCKAGE_IN_Q((&replica->waitq), rwait_cmd_next)
				CHECK_FOR_BLOCKAGE_IN_Q((&replica->read_waitq), rread_cmd_next)

				if(cmd_blocked == false) {
					TAILQ_INSERT_TAIL(&replica->sendq, rcmd, rsend_cmd_next);
					if(rcmd->opcode == ZVOL_OPCODE_READ) {
						read_cmd_sent = true;
					}
					pthread_cond_signal(&replica->r_cond);
				}
			}
			MTX_UNLOCK(&replica->r_mtx);
			if((rcmd->opcode == ZVOL_OPCODE_READ) && read_cmd_sent) {
				break;
			}
		}
		MTX_UNLOCK(&spec->rq_mtx);
	}
}

int
wait_for_fd(int epfd) {
	int event_count = 0;
	struct epoll_event event;
epollwait:
	event_count = epoll_wait(epfd, &event, 1, -1);

	if (event_count < 0) {
		if (errno == EINTR)
			goto epollwait;
		REPLICA_ERRLOG("epoll_wait ret %d err %d.. better to restart..\n", event_count, errno);
		goto epollwait;
	}
	if (!(event.events & EPOLLOUT)) {
		REPLICA_LOG("epoll err event %d on fd %d..\n", event.events, event.data.fd);
		return -1;
	} else {
		return 0;
	}
}

int
sendio(int epfd, int fd, rcommon_cmd_t *cmd, rcmd_t *rcmd) {
	char buf[sizeof(zvol_io_hdr_t) + sizeof(struct zvol_io_rw_hdr)];
	zvol_io_hdr_t *rio = (zvol_io_hdr_t *)(&(buf[0]));
	struct zvol_io_rw_hdr *rio_rw_hdr = (struct zvol_io_rw_hdr *)(&(buf[sizeof(zvol_io_hdr_t)]));
	int i = 0;
	int64_t rc = 0;
	int64_t nbytes = 0;
	nbytes = cmd->total_len;
	build_replica_io_hdr();
	rcmd->iov[0].iov_base = rio;
	if (cmd->opcode == ZVOL_OPCODE_WRITE)
		rcmd->iov[0].iov_len = sizeof(zvol_io_hdr_t) + sizeof(struct zvol_io_rw_hdr);
	else
		rcmd->iov[0].iov_len = sizeof(zvol_io_hdr_t);
	nbytes += rcmd->iov[0].iov_len;
	while (nbytes) {
write_to_socket:
		rc = writev(fd, rcmd->iov, rcmd->iovcnt+1);
		if (rc < 0) {
			if (errno == EAGAIN) {
				rc = wait_for_fd(epfd);
				if (rc == 0)
					goto write_to_socket;
				return -1;
			} else {
				REPLICA_LOG("write error %ld on fd %d\n", rc, fd);
				return -1;
			}
		} else if (rc == 0)
			REPLICA_LOG("fd %d returned 0\n", fd);
		nbytes -= rc;
		if (nbytes <= 0)
			break;
		for (i=0; i < rcmd->iovcnt + 1; i++) {
			if (rcmd->iov[i].iov_len != 0 && rcmd->iov[i].iov_len > (size_t)rc) {
				rcmd->iov[i].iov_base
					= (void *) (((uint8_t *)rcmd->iov[i].iov_base) + rc);
				rcmd->iov[i].iov_len -= rc;
				break;
			} else {
				rc -= rcmd->iov[i].iov_len;
				rcmd->iov[i].iov_len = 0;
			}
		}
	}
	return 0;
}

void *
replica_sender(void *arg)
{
	replica_t *replica = (replica_t *)arg;
	int rc, i, count;
	uint64_t ncmd;
	rcmd_t *rcmd = NULL, *pending_rcmd = NULL;
	rcommon_cmd_t *pending_cmd;
	struct epoll_event *events;
	bool cmd_blocked = false;

	events = calloc(MAXEVENTS, sizeof(*events));

	while(1) {
		count = epoll_wait(replica->sender_epfd, events, MAXEVENTS, -1);
		if (count == -1) {
			//TODO cleanup
			REPLICA_ERRLOG("got error on replica sender epfd\n");
			exit(1);
		}

		for (i = 0; i < count; rc++) {
			if (events[i].data.fd == replica->eventfd) {
				// got notification regarding new IO
				rc = eventfd_read(replica->eventfd, &ncmd);
				if (rc == -1) {
					//TODO cleanup
					REPLICA_ERRLOG("failed to read from replica eventfd\n");
				}
				REPLICA_LOG("got event count %lu\n", ncmd);

				while (get_num_entries_from_mempool(&replica->cmdq)) {
					rcmd = get_from_mempool(&replica->cmdq);
					if (!rcmd)
						break;

					if (!TAILQ_EMPTY(&replica->blockedq)) {
						TAILQ_INSERT_TAIL(&replica->blockedq, rcmd, rblocked_cmd_next);
					} else {
						cmd_blocked = false;
						rcmd_t *cmd = rcmd;	// to avoid change in macro : TODO
						CHECK_FOR_BLOCKAGE_IN_Q((&replica->sendq), rsend_cmd_next)
						CHECK_FOR_BLOCKAGE_IN_Q((&replica->waitq), rwait_cmd_next)

						if(cmd_blocked == false) {
							TAILQ_INSERT_TAIL(&replica->sendq, rcmd, rsend_cmd_next);
/*
	TODO
							if(rcmd->opcode == ZVOL_OPCODE_READ) {
								read_cmd_sent = true; //TODO handling of read command
							}
*/
						} else {
							TAILQ_INSERT_TAIL(&replica->blockedq, rcmd, rblocked_cmd_next);
						}
					}
				}
			} else {
				// got EPOLLOUT on data connection
				MTX_LOCK(&replica->r_mtx);
dequeue_rsendq:
				rcmd = TAILQ_FIRST(&replica->sendq);
				if(!rcmd) {
					if(replica->removed) {
						MTX_UNLOCK(&replica->r_mtx);
						break;
					}
					goto dequeue_rsendq;
				}
				TAILQ_REMOVE(&replica->sendq, rcmd, rsend_cmd_next);
				if(rcmd->opcode == ZVOL_OPCODE_READ) {
					rcmd->rrio_seq = ++replica->rrio_seq;
					TAILQ_INSERT_TAIL(&replica->read_waitq, rcmd, rread_cmd_next);
				} else {
					rcmd->wrio_seq = ++replica->wrio_seq;
//					rcommq_ptr = rcmd->rcommq_ptr;
					TAILQ_INSERT_TAIL(&replica->waitq, rcmd, rwait_cmd_next);
				}
				MTX_UNLOCK(&replica->r_mtx);
				rc = sendio(replica->sender_epfd, replica->iofd, rcmd->rcommq_ptr, rcmd);
				if(rc < 0) {
					remove_replica_from_list(replica->spec, replica->iofd);
					break;
				}
			}
		}
	}
	return(NULL);
}

void update_volstate(spec_t *spec)
{
	uint64_t max;
	replica_t *replica;

	if(((spec->healthy_rcount + spec->degraded_rcount >= spec->consistency_factor) &&
		(spec->healthy_rcount >= 1))||
		(spec->healthy_rcount  + spec->degraded_rcount 
			>= MAX(spec->replication_factor - spec->consistency_factor + 1, spec->consistency_factor))) {
		if (spec->ready == false)
		{
			max = 0;
			TAILQ_FOREACH(replica, &spec->rq, r_next)
				max = (max < replica->initial_checkpointed_io_seq) ?
				    replica->initial_checkpointed_io_seq : max;

			max = (max == 0) ? 10 : max + (1<<20);
			spec->io_seq = max;
		}
		spec->ready = true;
		pthread_cond_broadcast(&spec->rq_cond); //TODO
	} else {
		spec->ready = false;
	}
}

void clear_replica_cmd(spec_t *spec, rcmd_t *rep_cmd) {
	rcommon_cmd_t *rcommq_ptr = rep_cmd->rcommq_ptr;
	int luworker_id = rcommq_ptr->luworker_id;
	//TODO Add check for acks_received in read also, same as write
	if(rep_cmd->opcode == ZVOL_OPCODE_READ) {
		MTX_LOCK(&rcommq_ptr->rcommand_mtx);
		rcommq_ptr->state = CMD_EXECUTION_DONE;
		rcommq_ptr->status = -1;
		rcommq_ptr->completed = true;
		MTX_UNLOCK(&rcommq_ptr->rcommand_mtx);
		signal_luworker();
	} else if(rep_cmd->opcode == ZVOL_OPCODE_WRITE) {
		MTX_LOCK(&spec->rcommonq_mtx);
		MTX_LOCK(&rcommq_ptr->rcommand_mtx);
		rcommq_ptr->ios_aborted++;
		if(rcommq_ptr->acks_recvd + rcommq_ptr->ios_aborted
				== rcommq_ptr->copies_sent) {
			if(rcommq_ptr->state == CMD_ENQUEUED_TO_PENDINGQ)
				TAILQ_REMOVE(&spec->rcommon_pendingq, rcommq_ptr, pending_cmd_next);
			else if(rcommq_ptr->state == CMD_ENQUEUED_TO_WAITQ)
				TAILQ_REMOVE(&spec->rcommon_waitq, rcommq_ptr, wait_cmd_next);
			MTX_UNLOCK(&spec->rcommonq_mtx);
			rcommq_ptr->state = CMD_EXECUTION_DONE;
			if(rcommq_ptr->completed) {
				MTX_UNLOCK(&rcommq_ptr->rcommand_mtx);
				clear_rcomm_cmd(rcommq_ptr);
			} else {
				rcommq_ptr->completed = true;
				if (rcommq_ptr->acks_recvd < spec->consistency_factor) {
					rcommq_ptr->status = -1;
					signal_luworker();
				}
				MTX_UNLOCK(&rcommq_ptr->rcommand_mtx);
			}
		}
		else {
			MTX_UNLOCK(&rcommq_ptr->rcommand_mtx);
			MTX_UNLOCK(&spec->rcommonq_mtx);
		}
	}
}

#define EMPTY_Q_OF_REPLICA(queue, next) \
	while((rep_cmd = TAILQ_FIRST((queue)))) { \
		clear_replica_cmd(spec, rep_cmd); \
		TAILQ_REMOVE((queue), rep_cmd, next); \
		ios_aborted++; \
	}

int
remove_replica_from_list(spec_t *spec, int iofd) {
	replica_t *replica;
	int ios_aborted = 0;
	rcmd_t *rep_cmd = NULL;
	struct epoll_event event;
	MTX_LOCK(&spec->rq_mtx);
	TAILQ_FOREACH(replica, &spec->rq, r_next) {
		if(iofd == replica->iofd) {
			TAILQ_REMOVE(&spec->rq, replica, r_next);
			spec->replica_count--;
			update_volstate(spec);
			REPLICA_LOG("REMOVE REPLICA FROM LIST\n");
			MTX_LOCK(&replica->r_mtx);
			MTX_UNLOCK(&spec->rq_mtx);

			EMPTY_Q_OF_REPLICA((&replica->waitq), rwait_cmd_next)
			EMPTY_Q_OF_REPLICA((&replica->blockedq), rblocked_cmd_next)
			EMPTY_Q_OF_REPLICA((&replica->read_waitq), rread_cmd_next)

			EMPTY_MGMT_Q_OF_REPLICA(replica);
			replica->removed = true;
			pthread_cond_signal(&replica->r_cond);
			epoll_ctl(spec->receiver_epfd, EPOLL_CTL_DEL, replica->iofd, &event);
			MTX_UNLOCK(&replica->r_mtx);
//TODO	check if needed		cleanup_replica(replica);
			break;
		}
	}
	if (replica != NULL)
		REPLICA_LOG("%d IOs aborted for replica %d", ios_aborted, replica->id);
	else
		MTX_UNLOCK(&spec->rq_mtx);
	return 0;
}

/*
 * perform read/write on fd for 'len' according to state
 * sets 'errorno' if read/write operation returns < 0
 * closes fd if operation returns < 0 && errno != EAGAIN|EWOULDBLOCK|EINTR,
 * and sets fd_closed also closes fd if read return 0, i.e., EOF
 * returns number of bytes read/written
 */
static int64_t
perform_read_write_on_fd(int fd, uint8_t *data, uint64_t len, int state)
{
	int64_t rc = -1;
	uint64_t nbytes = 0;
	int read_cmd = 0;

	while(1) {
		switch (state) {
			case READ_IO_RESP_HDR:
			case READ_IO_RESP_DATA:
				rc = read(fd, data + nbytes, len - nbytes);
				read_cmd = 1;
				break;

			case WRITE_IO_SEND_HDR:
			case WRITE_IO_SEND_DATA:
				rc = write(fd, data + nbytes, len - nbytes);
				break;

			default:
				REPLICA_ERRLOG("received invalid state(%d)\n", state);
				errno = EINVAL;
				break;
		}

		if(rc < 0) {
			if (errno == EINTR) {
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return nbytes;
			} else {
				REPLICA_ERRLOG("received err %d on fd %d, closing it..\n", errno, fd);
				return -1;
			}
		} else if (rc == 0 && read_cmd) {
			REPLICA_ERRLOG("received EOF on fd %d, closing it..\n", fd);
			return -1;
		}

		nbytes += rc;
		if(nbytes == len) {
			break;
		}
	}

	return nbytes;
}

void unblock_blocked_cmds(replica_t *replica)
{
	rcmd_t *cmd, *pending_cmd;
	bool cmd_blocked, blocked = true;
	TAILQ_FOREACH(pending_cmd, &replica->blockedq, rblocked_cmd_next) {
		blocked = false;
		TAILQ_FOREACH(cmd, &replica->waitq, rwait_cmd_next) {
			check_for_blockage(cmd, pending_cmd);
			if(cmd_blocked == true) {
/*
				if(!cmd->ack_recvd) {
					blocked = true;
					break;
				}
*/
				blocked = true;
				break;
			}
		}
		if(blocked) {
			break;
		} else {
			/* we need to have similar check_for_blockage code for read_waitq */
			TAILQ_REMOVE(&replica->blockedq, pending_cmd, rblocked_cmd_next);
			TAILQ_INSERT_TAIL(&replica->sendq, pending_cmd, rsend_cmd_next);
			pthread_cond_signal(&replica->r_cond);
		}
	}
}

uint8_t *get_read_resp_data(void *data, uint64_t *datalen);

uint8_t *
get_read_resp_data(void *data, uint64_t *datalen)
{
	uint8_t *dataptr = (uint8_t *)data;
	uint64_t len = 0, parsed = 0;
	uint8_t *read_data;
	struct zvol_io_rw_hdr *io_hdr;
	zvol_io_hdr_t *hdr;

	hdr = (zvol_io_hdr_t *) data;
	dataptr = (uint8_t *)data + sizeof (zvol_io_hdr_t);

	while (parsed < hdr->len) {
		io_hdr = (struct zvol_io_rw_hdr *)dataptr;
		len += io_hdr->len;
		dataptr += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
		parsed += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
	}

	read_data = (uint8_t *)malloc(len);
	dataptr = (uint8_t *)data + sizeof (zvol_io_hdr_t);
	len = 0;
	parsed = 0;

	while (parsed < hdr->len) {
		io_hdr = (struct zvol_io_rw_hdr *)dataptr;
		dataptr += (sizeof(struct zvol_io_rw_hdr));
		memcpy(read_data + len, dataptr, io_hdr->len);
		len += io_hdr->len;
		dataptr += (io_hdr->len);
		parsed += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
	}
	*datalen = len;
	return read_data;
}

void
get_all_read_resp_data_chunk(replica_rcomm_resp_t *resp, struct io_data_chunk_list_t *io_chunk_list)
{
	zvol_io_hdr_t *hdr = &resp->io_resp_hdr;
	struct zvol_io_rw_hdr *io_hdr;
	uint8_t *dataptr = resp->data_ptr;
	uint64_t len = 0, parsed = 0;
	io_data_chunk_t  *io_chunk_entry;

	if (!TAILQ_EMPTY(io_chunk_list)) {
		io_chunk_entry = TAILQ_FIRST(io_chunk_list);

		while (parsed < hdr->len) {
			io_hdr = (struct zvol_io_rw_hdr *)dataptr;

			if (io_chunk_entry->io_num < io_hdr->io_num) {
				io_chunk_entry->data = dataptr + sizeof(struct zvol_io_rw_hdr) + len;
				io_chunk_entry->len = io_hdr->len;
			}

			len += io_hdr->len;
			dataptr += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
			parsed += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
			io_chunk_entry = TAILQ_NEXT(io_chunk_entry, io_data_chunk_next);
		}
	} else {
		while (parsed < hdr->len) {
			io_hdr = (struct zvol_io_rw_hdr *)dataptr;

			io_chunk_entry = malloc(sizeof(io_data_chunk_t));
			io_chunk_entry->io_num = io_hdr->io_num;
			io_chunk_entry->data = dataptr + sizeof(struct zvol_io_rw_hdr) + len;
			io_chunk_entry->len = io_hdr->len;
			TAILQ_INSERT_TAIL(io_chunk_list, io_chunk_entry, io_data_chunk_next);

			dataptr += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
			parsed += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
		}
	}
}

uint8_t *
process_chunk_read_resp(struct io_data_chunk_list_t  *io_chunk_list, uint64_t len)
{
	uint64_t parsed = 0;
	uint8_t *read_data;
	io_data_chunk_t *io_chunk_entry, *io_chunk_next;

	read_data = malloc(len);

	io_chunk_entry = TAILQ_FIRST(io_chunk_list);
	while (io_chunk_entry) {
		memcpy(read_data + parsed, io_chunk_entry->data, io_chunk_entry->len);
		parsed += io_chunk_entry->len;

		io_chunk_next = TAILQ_NEXT(io_chunk_entry, io_data_chunk_next);
		TAILQ_REMOVE(io_chunk_list, io_chunk_entry, io_data_chunk_next);
		free(io_chunk_entry);
		io_chunk_entry = io_chunk_next;
	}
	return read_data;
}

int
handle_read_resp(spec_t *spec, replica_t *replica)
{
	int luworker_id;
	bool io_found = false;
	rcmd_t *rep_cmd = NULL;
	rcommon_cmd_t *rcommq_ptr = NULL;
	zvol_io_hdr_t *io_rsp_hdr = replica->io_resp_hdr;
	void *data = replica->io_resp_data;
	uint64_t datalen = 0;
	uint8_t *dataptr = NULL;

	MTX_LOCK(&replica->r_mtx);
	//Find IO in read queue, signal luworker, and dequeue
	TAILQ_FOREACH(rep_cmd, &replica->read_waitq, rread_cmd_next) {
		if(io_rsp_hdr->io_seq == rep_cmd->io_seq) {
			io_found = true;
			rep_cmd->ack_recvd = true;
			TAILQ_REMOVE(&replica->read_waitq, rep_cmd, rread_cmd_next);
			MTX_UNLOCK(&replica->r_mtx);

//			if (io_rsp_hdr->status == ZVOL_OP_STATUS_OK)
//				dataptr = get_read_resp_data(io_rsp_hdr, data, &datalen);

			MTX_LOCK(&spec->rcommonq_mtx);
			rcommq_ptr = rep_cmd->rcommq_ptr;

			if (io_rsp_hdr->status == ZVOL_OP_STATUS_OK) {
				rcommq_ptr->acks_recvd++;
//				rcommq_ptr->resp_status[replica->id] = ZVOL_OP_STATUS_OK;
			} else {
				rcommq_ptr->ios_aborted++;
//				rcommq_ptr->resp_status[replica->id] = ZVOL_OP_STATUS_FAILED;
			}

//			resp_entry = calloc(1, sizeof(resp_entry));
//			resp_entry->data = dataptr;
//			resp_entry->len = datalen;
//			rcommq_ptr->resp_list[replica->id] = resp_entry;

			luworker_id = rcommq_ptr->luworker_id;
			put_to_mempool(&rcmd_mempool, rep_cmd);

			signal_luworker();
			return 0;
		}
	}
	MTX_UNLOCK(&replica->r_mtx);
	if(!io_found) {
		REPLICA_ERRLOG("error .. no io\n");
		/* a print or a stats */
		return -1;
	}
	return 0;
}

//else case for matching with copies_sent will be handled outside handle_consistency_met
#define handle_consistency_met() {\
	rcommq_ptr->status = 1; \
	TAILQ_REMOVE(&spec->rcommon_waitq, rcommq_ptr, wait_cmd_next); \
	if ((rcommq_ptr->acks_recvd + rcommq_ptr->ios_aborted) != rcommq_ptr->copies_sent) { \
		rcommq_ptr->state = CMD_ENQUEUED_TO_PENDINGQ; \
		TAILQ_INSERT_TAIL(&spec->rcommon_pendingq, rcommq_ptr, pending_cmd_next); \
	} \
	signal_luworker(); \
}

#define handle_all_resp_recvd() { \
	if(rcommq_ptr->state == CMD_ENQUEUED_TO_PENDINGQ) { \
		TAILQ_REMOVE(&spec->rcommon_pendingq, rcommq_ptr, pending_cmd_next); \
	} else if(rcommq_ptr->state == CMD_ENQUEUED_TO_WAITQ) { \
		TAILQ_REMOVE(&spec->rcommon_waitq, rcommq_ptr, wait_cmd_next); \
	} \
	rcommq_ptr->state = CMD_EXECUTION_DONE; \
	if(rcommq_ptr->completed) { \
		MTX_UNLOCK(&rcommq_ptr->rcommand_mtx); \
		clear_rcomm_cmd(rcommq_ptr); \
		rcommq_ptr = NULL; \
	} else { \
		rcommq_ptr->completed = true; \
		if (rcommq_ptr->acks_recvd < spec->consistency_factor) { \
			rcommq_ptr->status = -1; \
			signal_luworker(); \
		} \
		MTX_UNLOCK(&rcommq_ptr->rcommand_mtx); \
	} \
}

#if 0
int
assign_id_to_replica(replica_t *replica)
{
	int replica_id = -1;
	int rc = -1;
	spec_t *spec = replica->spec;
	int count;

	MTX_LOCK(&spec->rq_mtx);

	count = sizeof (spec->replica_index_list) * 8 - 1;
	do {
		if (!(spec->replica_index_list >> count & 1)) {
			replica_id = count;
			break;
		}
	} while (count--);

	if (replica_id == -1) {
		REPLICA_ERRLOG("Max replica limit reached\n");
	} else {
		spec->replica_index_list |= (1 << replica_id);
		replica->id = replica_id;
		rc = 0;
	}

	MTX_UNLOCK(&spec->rq_mtx);
	return rc;
}
#endif

int
handle_write_resp(spec_t *spec, replica_t *replica)
{
	int luworker_id;
	rcmd_t *rep_cmd = NULL;
	rcommon_cmd_t *rcommq_ptr;
	zvol_io_hdr_t *io_rsp = replica->io_resp_hdr;

	MTX_LOCK(&replica->r_mtx);
	TAILQ_FOREACH(rep_cmd, &replica->waitq, rwait_cmd_next) {
		if(rep_cmd->io_seq == io_rsp->io_seq) {
			break;
		}
	}

	if(rep_cmd == NULL) {
		REPLICA_ERRLOG("rep_cmd not found io_seq:%lu\n", io_rsp->io_seq);
		MTX_UNLOCK(&replica->r_mtx);
		return -1;
	}

	rep_cmd->ack_recvd = true;
	rep_cmd->status = io_rsp->status;
	rcommq_ptr = rep_cmd->rcommq_ptr;
	TAILQ_REMOVE(&replica->waitq, rep_cmd, rwait_cmd_next);
	MTX_UNLOCK(&replica->r_mtx);

	//below lock/unlock sequence is to make sure atomically that
	//copies_sent is incremented by adding command to all replicas.
	//this is actually required only in the case of
	//(rcommq_ptr->acks_recvd + rcommq_ptr->ios_aborted == rcommq_ptr->copies_sent)
	MTX_LOCK(&spec->rq_mtx);
	MTX_UNLOCK(&spec->rq_mtx);


	if (rep_cmd->status == ZVOL_OP_STATUS_OK) {
		rcommq_ptr->acks_recvd++;
//		rcommq_ptr->resp_status[replica->id] = ZVOL_OP_STATUS_OK;
	} else {
		rcommq_ptr->ios_aborted++;
//		rcommq_ptr->resp_status[replica->id] = ZVOL_OP_STATUS_FAILED;
	}

	luworker_id = rcommq_ptr->luworker_id;

#if 0
	if (rcommq_ptr->acks_recvd == spec->consistency_factor) {
	//above condition can be met multiple times, so, below condition is required
		if (rcommq_ptr->status == 0)
			handle_consistency_met();
	}
	if (rcommq_ptr->acks_recvd + rcommq_ptr->ios_aborted
	    == rcommq_ptr->copies_sent) {
		handle_all_resp_recvd();
	}
	else
		MTX_UNLOCK(&rcommq_ptr->rcommand_mtx);
	MTX_UNLOCK(&spec->rcommonq_mtx);
#endif
	put_to_mempool(&rcmd_mempool, rep_cmd);
	rep_cmd = NULL;
	signal_luworker();

	//TODO if io_rsp status is not OK, replica need to be cleared
	return 0;
}

typedef struct io_event {
	int fd;
	int *state;
	zvol_io_hdr_t *io_hdr;
	void **io_data;
	int *byte_count;
} io_event_t;

int read_io_resp(spec_t *spec, replica_t *replica, io_event_t *revent);

//Receive IO responses in this thread
void *
replica_receiver(void *arg)
{
	spec_t *spec = (spec_t *)arg;
	replica_t *replica;
	int i, event_count, replica_count = 0, ret;
	struct epoll_event event, *events;
	io_event_t revent;

	//Create a new epoll epfd
	int epfd = spec->receiver_epfd;
	events = calloc(MAXEVENTS, sizeof(event));
	while(1) {
		if(replica_count == 0) {
			MTX_LOCK(&spec->rq_mtx);
			if(spec->replica_count == 0) {
				//Wait until at least one IO connection has been made to a registered replica
				pthread_cond_wait(&spec->rq_cond, &spec->rq_mtx);
			}
			MTX_UNLOCK(&spec->rq_mtx);
		}
		//Wait for events on all iofds
		event_count = epoll_wait(epfd, events, MAXEVENTS, -1);
		for(i=0; i< event_count; i++) {
			//Remove the replica from queue
			if (!(events[i].events & EPOLLIN)) {
				remove_replica_from_list(spec, events[i].data.fd);
				REPLICA_LOG("epoll error\n");
				continue;
			} else {
				MTX_LOCK(&spec->rq_mtx);
				TAILQ_FOREACH(replica, &spec->rq, r_next) {
					if(events[i].data.fd == replica->iofd) {
						break;
					}
				}
				MTX_UNLOCK(&spec->rq_mtx);

				revent.fd = events[i].data.fd;
				revent.state = &(replica->io_state);
				revent.io_hdr = replica->io_resp_hdr;
				revent.io_data = &(replica->io_resp_data);
				revent.byte_count = &(replica->io_read);

				ret = read_io_resp(spec, replica, &revent);

				if (ret != 0) {
					MTX_LOCK(&replica->r_mtx);
					unblock_blocked_cmds(replica);
					MTX_UNLOCK(&replica->r_mtx);
				}
			}
		}
	}
	close(epfd);
	return NULL;
}

/* creates replica entry and adds to spec's rwaitq list after creating mgmt connection */
replica_t *
create_replica_entry(spec_t *spec, int epfd, int mgmt_fd)
{
	replica_t *replica = NULL;
	int rc;

	replica = (replica_t *)malloc(sizeof(replica_t));
	if (!replica)
		return NULL;

	replica->epfd = epfd;
	replica->mgmt_fd = mgmt_fd;
	TAILQ_INIT(&(replica->mgmt_cmd_queue));
	replica->initial_checkpointed_io_seq = 0;
	replica->mgmt_io_resp_hdr = malloc(sizeof(zvol_io_hdr_t));
	replica->mgmt_io_resp_data = NULL;
	MTX_LOCK(&spec->rq_mtx);
	TAILQ_INSERT_TAIL(&spec->rwaitq, replica, r_waitnext);
	MTX_UNLOCK(&spec->rq_mtx);

	replica->mgmt_eventfd2 = -1;
	replica->iofd = -1;
	replica->spec = spec;

	rc = pthread_mutex_init(&replica->r_mtx, NULL); //check
	if (rc != 0) {
		REPLICA_ERRLOG("pthread_mutex_init() failed errno:%d\n", errno);
		return NULL;
	}
	rc = pthread_cond_init(&replica->r_cond, NULL); //check
	if (rc != 0) {
		REPLICA_ERRLOG("pthread_cond_init() failed errno:%d\n", errno);
		return NULL;
	}
	return replica;
}

void *replica_thread(void *);
/*
 * updates replica entry with IP/port
 * removes from spec's rwaitq, adds to rq after data connection
 * starts sender thread to send IOs to replica
 * Note: Locks in update_replica_entry are avoided since update_replica_entry is being
 *	 executed once only during handshake with replica.
 */
int
update_replica_entry(spec_t *spec, replica_t *replica, int iofd)
{
	int rc;
	zvol_io_hdr_t *rio;
	pthread_t r_thread;
	zvol_io_hdr_t *ack_hdr;
	mgmt_ack_t *ack_data;
	mgmt_ack_t handshake_resp;
	int i;

	ack_hdr = replica->mgmt_io_resp_hdr;	
	ack_data = (mgmt_ack_t *)replica->mgmt_io_resp_data;

	TAILQ_INIT(&replica->sendq);
	TAILQ_INIT(&replica->waitq);
	TAILQ_INIT(&replica->read_waitq);
	TAILQ_INIT(&replica->blockedq);
	TAILQ_INIT(&replica->readyq);

	replica->ongoing_io = NULL;
	replica->ongoing_io_len = 0;
	replica->ongoing_io_buf = NULL;
	replica->iofd = iofd;
	replica->ip = malloc(strlen(ack_data->ip)+1);
	strcpy(replica->ip, ack_data->ip);
	replica->port = ack_data->port;
	replica->state = ZVOL_STATUS_DEGRADED;
	replica->initial_checkpointed_io_seq = ack_hdr->checkpointed_io_seq;

	replica->pool_guid = ack_data->pool_guid;
	replica->zvol_guid = ack_data->zvol_guid;

	replica->least_recvd = 0;
	replica->wrio_seq = 0;
	replica->rrio_seq = 0;
	replica->spec = spec;
	replica->io_resp_hdr = (zvol_io_hdr_t *)malloc(sizeof(zvol_io_hdr_t));
	replica->io_resp_data = NULL;
	replica->io_state = READ_IO_RESP_HDR;
	replica->io_read = 0;
	replica->removed = false;

	rio = (zvol_io_hdr_t *)malloc(sizeof(zvol_io_hdr_t));
	rio->opcode = ZVOL_OPCODE_HANDSHAKE;
	rio->io_seq = 0;
	rio->offset = 0;
	rio->len = strlen(spec->lu->volname);
	rio->version = REPLICA_VERSION;

	rc = write(replica->iofd, rio, sizeof(zvol_io_hdr_t));
	write(replica->iofd, spec->lu->volname, strlen(spec->lu->volname));
	read(replica->iofd, &handshake_resp, sizeof (handshake_resp));
	free(rio);

	if(init_mempool(&replica->cmdq, rcmd_mempool_count, sizeof (rcmd_t), 0,
	    "replica_cmd_mempool", NULL, NULL, NULL, false)) {
		REPLICA_ERRLOG("Failed to initialize replica cmdq\n");
		exit(EXIT_FAILURE);
	}

	rc = make_socket_non_blocking(iofd);
	if (rc == -1) {
		REPLICA_ERRLOG("make_socket_non_blocking() failed errno:%d\n", errno);
		replica->iofd = -1;
		close(iofd);
		return -1;
	}

	rc = pthread_create(&r_thread, NULL, &replica_thread,
			(void *)replica);
	if (rc != 0) {
		ISTGT_ERRLOG("pthread_create(r_thread) failed\n");
		replica->iofd = -1;
		close(iofd);
		return -1;
	}

	for (i = 0; (i < 10) && (replica->mgmt_eventfd2 == -1); i++)
		sleep(1);

	if (replica->mgmt_eventfd2 == -1) {
		ISTGT_ERRLOG("unable to set mgmteventfd2 for more than 10 seconds for replica %s %d..\n", replica->ip, replica->port);
		MTX_LOCK(&replica->r_mtx);
		replica->iofd = -1;
		MTX_UNLOCK(&replica->r_mtx);
		close(iofd);
		return -1;
	}

	MTX_LOCK(&spec->rq_mtx);

	spec->replica_count++;
	spec->degraded_rcount++;
	TAILQ_REMOVE(&spec->rwaitq, replica, r_waitnext);
	TAILQ_INSERT_TAIL(&spec->rq, replica, r_next);
	if(spec->replica_count == 1)
		pthread_cond_signal(&spec->rq_cond);
	update_volstate(spec);

	MTX_UNLOCK(&spec->rq_mtx);


#if 0
	event.data.fd = replica->iofd;
	event.events = EPOLLIN | EPOLLET;
	rc = epoll_ctl(spec->receiver_epfd, EPOLL_CTL_ADD, iofd, &event);
	if (rc == -1) {
		/*
		 * TODO: need to cleanup replica and also update volstate here
		 */
		return NULL;
	}

	replica->sender_epfd = epoll_create1(0);
	event.data.fd = replica->iofd;
	event.events = EPOLLOUT;
	rc = epoll_ctl(replica->sender_epfd, EPOLL_CTL_ADD, replica->iofd, &event);
	if (rc == -1) {
		REPLICA_LOG("epoll_ctl_add failed errno:%d\n", errno);
		/*
		 * TODO: need to cleanup replica and also update volstate here
		 */
		return NULL;
	}

	rc = pthread_create(&replica_sender_thread, NULL, &replica_sender,
			(void *)replica);
	if (rc != 0) {
		ISTGT_ERRLOG("pthread_create(replicator_thread) failed\n");
		/*
		 * TODO: need to cleanup replica and also update volstate here
		 */
		return NULL;
	}
#endif
	return 0;
}

static void
cleanup_replica(replica_t *replica)
{
	MTX_LOCK(&replica->r_mtx);
	if(replica->mgmt_fd != -1)  {
		close(replica->mgmt_fd);
		replica->mgmt_fd = -1;
	}

	if(replica->iofd != -1)  {
		close(replica->iofd);
		replica->iofd = -1;
	}

	if(replica->sender_epfd != -1) {
		close(replica->sender_epfd);
		replica->sender_epfd = -1;
	}

	if(replica->io_resp_hdr) {
		free(replica->io_resp_hdr);
	}

	if(replica->ip) {
		free(replica->ip);
	}

	free(replica->mgmt_io_resp_hdr);
	if (replica->mgmt_io_resp_data) {
		free(replica->mgmt_io_resp_data);
		replica->mgmt_io_resp_data = NULL;
	}

	MTX_UNLOCK(&replica->r_mtx);

	pthread_mutex_destroy(&replica->r_mtx);
	pthread_cond_destroy(&replica->r_cond);
	free(replica);
}

static int
send_replica_handshake_query(replica_t *replica, spec_t *spec)
{
	zvol_io_hdr_t *rmgmtio = NULL;
	size_t data_len = 0;
	uint8_t *data;
	zvol_op_code_t mgmt_opcode = ZVOL_OPCODE_HANDSHAKE;
	mgmt_cmd_t *mgmt_cmd;

	mgmt_cmd = malloc(sizeof(mgmt_cmd_t));

	data_len = strlen(spec->lu->volname) + 1;

	build_replica_mgmt_hdr();

	data = (uint8_t *)malloc(data_len);
	snprintf((char *)data, data_len, "%s", spec->lu->volname);

	mgmt_cmd->io_hdr = rmgmtio;
	mgmt_cmd->io_bytes = 0;
	mgmt_cmd->data = data;
	mgmt_cmd->mgmt_cmd_state = WRITE_IO_SEND_HDR;

	MTX_LOCK(&replica->r_mtx);
	TAILQ_INSERT_TAIL(&replica->mgmt_cmd_queue, mgmt_cmd, mgmt_cmd_next);
	MTX_UNLOCK(&replica->r_mtx);

	return 0;
}

static int
send_replica_status_query(replica_t *replica, spec_t *spec)
{
	zvol_io_hdr_t *rmgmtio = NULL;
	size_t data_len;
	uint8_t *data;
	zvol_op_code_t mgmt_opcode = ZVOL_OPCODE_REPLICA_STATUS;
	mgmt_cmd_t *mgmt_cmd;

	mgmt_cmd = malloc(sizeof(mgmt_cmd_t));
	data_len = strlen(spec->lu->volname) + 1;
	build_replica_mgmt_hdr();

	data = (uint8_t *)malloc(data_len);
	snprintf((char *)data, data_len, "%s", spec->lu->volname);

	mgmt_cmd->io_hdr = rmgmtio;
	mgmt_cmd->io_bytes = 0;
	mgmt_cmd->data = data;
	mgmt_cmd->mgmt_cmd_state = WRITE_IO_SEND_HDR;

	MTX_LOCK(&replica->r_mtx);
	TAILQ_INSERT_TAIL(&replica->mgmt_cmd_queue, mgmt_cmd, mgmt_cmd_next);
	MTX_UNLOCK(&replica->r_mtx);

	return handle_write_data_event(replica);
}


/*
 * ask_replica_status will send replica_status query to all degraded replica
 */
static void
ask_replica_status_all(spec_t *spec)
{
	int ret;
	replica_t *replica;

	MTX_LOCK(&spec->rq_mtx);
	TAILQ_FOREACH(replica, &spec->rq, r_next) {
		if (replica->state == ZVOL_STATUS_HEALTHY) {
			continue;
		}

		ret = send_replica_status_query(replica, spec);
		if (ret == -1) {
			REPLICA_ERRLOG("send mgmtIO for status failed on "
			    "replica(%d, %s:%d) .. stopped sendign status "
			    "in this iteration\n", replica->id,
			    replica->ip, replica->port);
			MTX_UNLOCK(&spec->rq_mtx);
			handle_mgmt_conn_error(replica);
			return;
		}
	}
	MTX_UNLOCK(&spec->rq_mtx);
}

static int
update_replica_status(spec_t *spec, replica_t *replica)
{
	zrepl_status_ack_t *repl_status;
	replica_state_t last_status;

	repl_status = (zrepl_status_ack_t *)replica->mgmt_io_resp_data;

	MTX_LOCK(&spec->rq_mtx);
	MTX_LOCK(&replica->r_mtx);
	last_status = replica->state;
	replica->state = (replica_state_t) repl_status->state;
	MTX_UNLOCK(&replica->r_mtx);

	if(last_status != repl_status->state) {
		if (repl_status->state == ZVOL_STATUS_DEGRADED) {
			spec->degraded_rcount++;
			spec->healthy_rcount--;
		} else if (repl_status->state == ZVOL_STATUS_HEALTHY) {
			spec->degraded_rcount--;
			spec->healthy_rcount++;
		}
		update_volstate(spec);
	}
	MTX_UNLOCK(&spec->rq_mtx);
	return 0;
}

/*
 * forms data connection to replica, updates replica entry
 */
int
zvol_handshake(spec_t *spec, replica_t *replica)
{
	int rc, iofd;
	zvol_io_hdr_t *ack_hdr;
	mgmt_ack_t *ack_data;

	ack_hdr = replica->mgmt_io_resp_hdr;	
	ack_data = (mgmt_ack_t *)replica->mgmt_io_resp_data;

	if (ack_hdr->status != ZVOL_OP_STATUS_OK) {
		REPLICA_ERRLOG("mgmt_ack status is not ok..\n");
		return -1;
	}

	if(strcmp(ack_data->volname, spec->lu->volname) != 0) {
		REPLICA_ERRLOG("volname %s not matching with spec %s volname\n",
		    ack_data->volname, spec->lu->volname);
		return -1;
	}

	if((iofd = cstor_ops.conn_connect(ack_data->ip, ack_data->port)) < 0) {
		REPLICA_ERRLOG("conn_connect() failed errno:%d\n", errno);
		return -1;
	}

	rc = update_replica_entry(spec, replica, iofd);

	return rc;
}

typedef struct mgmt_event {
	int fd;
	replica_t *r_ptr;
} mgmt_event_t;

/*
 * accepts (mgmt) connections on which handshake and other management IOs are sent
 * sends handshake IO to start handshake on accepted (mgmt) connection
 */
void
accept_mgmt_conns(int epfd, int sfd) {
	struct epoll_event event;
	int rc, rcount=0;
	spec_t *spec;
	int mgmtfd[MAXREPLICA];
	char *buf = malloc(BUFSIZE);
	int mgmt_fd;
	mgmt_event_t *mevent1, *mevent2;

	mevent1 = (mgmt_event_t *)malloc(sizeof(mgmt_event_t));
	mevent2 = (mgmt_event_t *)malloc(sizeof(mgmt_event_t));

	while (1) {
		struct sockaddr saddr;
		socklen_t slen;
		char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
		replica_t *replica = NULL;

		slen = sizeof(saddr);
		mgmt_fd = accept(sfd, &saddr, &slen);
		if (mgmt_fd == -1) {
			if((errno != EAGAIN) && (errno != EWOULDBLOCK))
				REPLICA_ERRLOG("accept() failed on fd %d, errno:%d.. better to restart listener..", sfd, errno);
			break;
		}

		rc = getnameinfo(&saddr, slen,
				hbuf, sizeof(hbuf),
				sbuf, sizeof(sbuf),
				NI_NUMERICHOST | NI_NUMERICSERV);
		if (rc == 0) {
			mgmtfd[rcount] = mgmt_fd;
			rcount++;
			REPLICA_LOG("Accepted connection on descriptor %d "
					"(host=%s, port=%s)\n", mgmt_fd, hbuf, sbuf);
		}
		rc = make_socket_non_blocking(mgmt_fd);
		if (rc == -1) {
			REPLICA_ERRLOG("make_socket_non_blocking() failed on fd %d, errno:%d.. closing it..", mgmt_fd, errno);
			close(mgmt_fd);
			continue;
		}

                MTX_LOCK(&specq_mtx);
                TAILQ_FOREACH(spec, &spec_q, spec_next) {
			// Since we are supporting single spec per controller
			// we will continue using first spec only	
                        break;
                }
                MTX_UNLOCK(&specq_mtx);

		/*
		 * As of now, we are supporting single spec_t per target
		 * So, we can assign spec to replica here.
		 * TODO: In case of multiple spec, asignment of spec to replica
		 * 	 should be handled in update_replica_entry func according to
		 * 	 volume name provided by replica.
		 */
		replica = create_replica_entry(spec, epfd, mgmt_fd);
		if (!replica) {
			REPLICA_ERRLOG("Failed to create replica for fd %dclosing it..", mgmt_fd);
			close(mgmt_fd);
			continue;
		}

		replica->mgmt_eventfd1 = eventfd(0, EFD_NONBLOCK);
		if (replica->mgmt_eventfd1 < 0) {
			perror("r_mgmt_eventfd1");
			return NULL;
		}

		mevent1->fd = replica->mgmt_eventfd1;
		mevent1->r_ptr = replica;
		event.data.ptr = mevent1;
		event.events = EPOLLIN;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, replica->mgmt_eventfd1, &event);
		if(rc == -1) {
			REPLICA_ERRLOG("epoll_ctl() failed on fd %d, errno:%d.. closing it..", mgmt_fd, errno);
			cleanup_replica(replica);
			close(mgmt_fd);
			continue;
		}

		mevent2->fd = mgmt_fd;
		mevent2->r_ptr = replica;
		event.data.ptr = mevent2;
		event.events = EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLET | EPOLLOUT | EPOLLRDHUP;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, mgmt_fd, &event);
		if(rc == -1) {
			REPLICA_ERRLOG("epoll_ctl() failed on fd %d, errno:%d.. closing it..", mgmt_fd, errno);
			cleanup_replica(replica);
			close(mgmt_fd);
			continue;
		}

		send_replica_handshake_query(replica, spec);

	}
	free(buf);
}

/*
 * breaks if fd is closed or drained recv buf of fd
 * updates amount of data read to continue next time
 */
#define CHECK_AND_ADD_BREAK_IF_PARTIAL(io_read, count, reqlen, donecount) \
{ \
	if (count == -1) { \
		donecount = -1; \
		break; \
	} \
	if (count != reqlen) { \
		(io_read) += count; \
		break; \
	} \
}

/*
 * write_io_data will write IOs to mgmt connection only..
 * initial state is write_io_send_hdr, which will write header.
 * it transitions to write_io_send_data based on length in hdr.
 * once data is written, it will change io_state to READ_IO_RESP_HDR
 * to read data from replica.
 */
static int
write_io_data(replica_t *replica, io_event_t *wevent)
{
	int fd = wevent->fd;
	int *state = wevent->state;
	zvol_io_hdr_t *write_hdr = wevent->io_hdr;
	void **data = wevent->io_data;
	int *write_count = wevent->byte_count;
	uint64_t reqlen, count;
	int donecount = 0;
	(void)replica;

	switch(*state) {
		case WRITE_IO_SEND_HDR:
			reqlen = sizeof (zvol_io_hdr_t) - (*write_count);
			count = perform_read_write_on_fd(fd,
			    ((uint8_t *)write_hdr) + (*write_count), reqlen, *state);
			CHECK_AND_ADD_BREAK_IF_PARTIAL((*write_count), count, reqlen, donecount);

			*write_count = 0;
			*state = WRITE_IO_SEND_DATA;

		case WRITE_IO_SEND_DATA:
			reqlen = write_hdr->len - (*write_count);
			if (reqlen != 0) {
				count = perform_read_write_on_fd(fd,
				    ((uint8_t *)(*data)) + (*write_count), reqlen, *state);
				CHECK_AND_ADD_BREAK_IF_PARTIAL((*write_count), count, reqlen, donecount);
			}
			free(*data);
			*data = NULL;
			*write_count = 0;
			donecount++;
			*state = READ_IO_RESP_HDR;
			break;
	}

	return donecount;
}


/*
 * initial state is read io_resp_hdr, which reads IO response.
 * it transitions to read io_resp_data based on length in hdr.
 * once data is handled, it goes to read hdr which can be new response.
 * this goes on until EAGAIN or connection gets closed.
 */
int
read_io_resp(spec_t *spec, replica_t *replica, io_event_t *revent)
{
	int fd = revent->fd;
	int *state = revent->state;
	zvol_io_hdr_t *resp_hdr = revent->io_hdr;
	void **resp_data = revent->io_data;
	int *read_count = revent->byte_count;
	uint64_t reqlen, count;
	int errorno = 0, fd_closed = 0;
	int donecount = 0;

	switch(*state) {
		case READ_IO_RESP_HDR:
read_io_resp_hdr:
			reqlen = sizeof (zvol_io_hdr_t) - (*read_count);
			count = perform_read_write_on_fd(fd,
			    ((uint8_t *)resp_hdr) + (*read_count), reqlen, *state);
			CHECK_AND_ADD_BREAK_IF_PARTIAL((*read_count), count, reqlen, donecount);

			*read_count = 0;
			if (resp_hdr->opcode == ZVOL_OPCODE_WRITE)
				resp_hdr->len = 0;
			if (resp_hdr->len != 0) {
				(*resp_data) = malloc(resp_hdr->len);
			}
			*state = READ_IO_RESP_DATA;

		case READ_IO_RESP_DATA:
			reqlen = resp_hdr->len - (*read_count);
			if (reqlen != 0) {
				count = perform_read_write_on_fd(fd,
				    ((uint8_t *)(*resp_data)) + (*read_count), reqlen, *state);
				CHECK_AND_ADD_BREAK_IF_PARTIAL((*read_count), count, reqlen, donecount);
			}

			*read_count = 0;

			switch (resp_hdr->opcode) {
				case ZVOL_OPCODE_READ:
					handle_read_resp(spec, replica);
					replica->io_resp_data = NULL;
					break;

				case ZVOL_OPCODE_WRITE:
					if ((*resp_data) != NULL)
						REPLICA_ERRLOG("resp_data should be NULL for write IOs\n");
					handle_write_resp(spec, replica);
					break;

				case ZVOL_OPCODE_HANDSHAKE:
					if(resp_hdr->len != sizeof (mgmt_ack_t))
						REPLICA_ERRLOG("mgmt_ack_len %lu not matching with size of mgmt_ack_data..\n",
						    resp_hdr->len);

					/* dont process handshake on data connection */
					if (fd != replica->iofd)
						rc = zvol_handshake(spec, replica);
					free(*resp_data);
					if (rc == -1)
						donecount = -1;
					break;

				case ZVOL_OPCODE_REPLICA_STATUS:
					if(resp_hdr->len != sizeof (zrepl_status_ack_t))
						REPLICA_ERRLOG("replica_state_t length %lu is not matching with size of repl status data..\n",
							resp_hdr->len);

					/* replica status must come from mgmt connection */
					if (fd != replica->iofd)
						update_replica_status(spec, replica);
					free(*resp_data);
					break;

				default:
					REPLICA_NOTICELOG("unsupported opcode(%d) received..\n", resp_hdr->opcode);
					break;
			}
			*resp_data = NULL;
			donecount++;
			*state = READ_IO_RESP_HDR;
			goto read_io_resp_hdr;
	}
	return donecount;
}

/*
 * write data on management fd
 */
int
handle_write_data_event(replica_t *replica)
{
	io_event_t wevent;
	int rc = -1;
	mgmt_cmd_t *mgmt_cmd;

	MTX_LOCK(&replica->r_mtx);
	mgmt_cmd = TAILQ_FIRST(&replica->mgmt_cmd_queue);
	if (!mgmt_cmd) {
		MTX_UNLOCK(&replica->r_mtx);
		return;
	}

	if ((mgmt_cmd->mgmt_cmd_state != WRITE_IO_SEND_HDR &&
		mgmt_cmd->mgmt_cmd_state != WRITE_IO_SEND_DATA)) {
		MTX_UNLOCK(&replica->r_mtx);
		REPLICA_ERRLOG("write IO is in wait state on mgmt connection..");
		return rc; //TODO if returned 0
	}

	MTX_UNLOCK(&replica->r_mtx);

	wevent.fd = replica->mgmt_fd;
	wevent.state = &(mgmt_cmd->mgmt_cmd_state);
	wevent.io_hdr = mgmt_cmd->io_hdr;
	wevent.io_data = (void **)(&(mgmt_cmd->data));
	wevent.byte_count = &(mgmt_cmd->io_bytes);

	rc = write_io_data(replica, &wevent);
	return rc;
}

void
handle_mgmt_cmd(replica_t *r, mgmt_cmd_t *mgmt_cmd)
{
	r->mgmt_io_resp_hdr->status = ZVOL_OP_STATUS_FAILED;
	switch (mgmt_cmd->io_hdr->opcode)
	{
		case ZVOL_OPCODE_HANDSHAKE:
			break;
		case ZVOL_OPCODE_REPLICA_STATUS:
			break;
	}
	free(mgmt_cmd->io_hdr);
	free(mgmt_cmd->data);
	free(mgmt_cmd);
}

void
respond_with_error_for_all_outstanding_mgmt_ios(replica_t *r)
{
	mgmt_cmd_t *mgmt_cmd, *next_cmd;

	MTX_LOCK(&r->r_mtx);
	mgmt_cmd = TAILQ_FIRST(&r->mgmt_cmd_queue);
	while (mgmt_cmd != NULL) {
		next_cmd = TAILQ_NEXT(mgmt_cmd, mgmt_cmd_next);
		TAILQ_REMOVE(&r->mgmt_cmd_queue, mgmt_cmd, mgmt_cmd_next);
		handle_mgmt_cmd(r, mgmt_cmd);
		mgmt_cmd = next_cmd;
	}
	MTX_UNLOCK(&r->r_mtx);
}

void
inform_data_conn(replica_t *r)
{
	uint64_t num = 1;
	r->disconnect_conn = 1;
	write(r->mgmt_eventfd2, &num, sizeof (num));
}

void
free_replica(replica_t *r)
{
	/* TOOD: when to remove from rwaitq of spec */
	pthread_mutex_destroy(&r->r_mtx);
	pthread_cond_destroy(&r->r_cond);

	free(r->io_resp_hdr);
	free(r);
}

void close_fd(int epollfd, int fd)
{
	if (epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL) == -1) {
		perror("epoll_ctl");
		return;
	}
	close(fd);
}

void close_mgmt_eventfd2(replica_t *r)
{
	int mgmt_fd = r->mgmt_fd;
	if (epoll_ctl(r->epollfd, EPOLL_CTL_DEL, r->mgmt_fd, NULL) == -1) {
		perror("epoll_ctl");
		return;
	}
	r->mgmt_fd = -1;
	close(mgmt_fd);
}


void
handle_mgmt_conn_error(replica_t *r)
{
	int epollfd = r->epfd;
	int mgmtfd, mgmt_eventfd1;

	MTX_LOCK(&r->r_mtx);

	r->conn_closed++;
	if (r->conn_closed != 2) {
		//ASSERT(r->conn_closed == 1);
		/* case where error happened while sending HANDSHAKE or sending is successful but error from zvol_handshake */
		if ((r->iofd == -1) || (r->mgmt_eventfd2 == -1)) {
			TAILQ_REMOVE(&r->spec->rwaitq, r, r_waitnext); //TODO rwait_next);
			r->conn_closed++;
		} else {
			inform_data_conn(r);
		}
	} else {
		pthread_cond_signal(&r->r_cond);
	}

	mgmtfd = r->mgmt_fd;
	r->mgmt_fd = -1;
	MTX_UNLOCK(&r->r_mtx);

	close_fd(epollfd, mgmtfd);

	MTX_LOCK(&r->r_mtx);
	if (r->conn_closed != 2) {
		//ASSERT(r->conn_closed == 1);
		pthread_cond_wait(&r->r_cond, &r->r_mtx);
	}
	MTX_UNLOCK(&r->r_mtx);

	/* this need to be called after replica is removed from spec list
	 * data_conn thread should have removed from spec list as conn_closed is 2 */
	respond_with_error_for_all_outstanding_mgmt_ios(r);

	mgmt_eventfd1 = r->mgmt_eventfd1;
	r->mgmt_eventfd1 = -1;
	close(mgmt_eventfd1);

	if (r->mgmt_eventfd2 != -1)
		free_replica(r);
}

int
handle_mgmt_event_fd(replica_t *replica)
{
	int rc = 0;

	do_drainfd(replica->mgmt_eventfd1);

	MTX_LOCK(&replica->r_mtx);
	if (replica->disconnect_conn == 1) {
		MTX_UNLOCK(&replica->r_mtx);
		handle_mgmt_conn_error(replica);
		return rc;
	}
	MTX_UNLOCK(&replica->r_mtx);

	rc = handle_write_data_event(replica);
	return rc;
}

/*
 * reads data on management fd
 */
int
handle_read_data_event(replica_t *replica)
{
	io_event_t revent;
	mgmt_cmd_t *mgmt_cmd;
	int rc = 0;

	MTX_LOCK(&replica->r_mtx);
	mgmt_cmd = TAILQ_FIRST(&replica->mgmt_cmd_queue);
	if (!(mgmt_cmd != NULL &&
		(mgmt_cmd->mgmt_cmd_state == READ_IO_RESP_HDR ||
		mgmt_cmd->mgmt_cmd_state == READ_IO_RESP_DATA))) {
		MTX_UNLOCK(&replica->r_mtx);
		/*
		 * Though we didn't send any IO query on management connection,
		 * We have a read event on management connection. Thats an error as
		 * management connection is not working in stateful manner. So we
		 * will print error message and does cleanup
		 */
		REPLICA_ERRLOG("unexpected read IO on mgmt connection..");
		return (-1);
	}

	MTX_UNLOCK(&replica->r_mtx);

	revent.fd = replica->mgmt_fd;
	revent.state = &(mgmt_cmd->mgmt_cmd_state);
	revent.io_hdr = replica->mgmt_io_resp_hdr;
	revent.io_data = (void **)(&(replica->mgmt_io_resp_data));
	revent.byte_count = &(mgmt_cmd->io_bytes);

	rc = read_io_resp(replica->spec, replica, &revent);
	if (rc > 0) {
		if (rc > 1)
			REPLICA_NOTICELOG("read performed on management connection for more"
			    " than one IOs..");

		MTX_LOCK(&replica->r_mtx);
		clear_mgmt_cmd(replica, mgmt_cmd);
		MTX_UNLOCK(&replica->r_mtx);
		rc = handle_write_data_event(replica);
	}
	return (rc);
}

/* MACRO from istgt_lu_disk.c */
#define timesdiff(_st, _now, _re)                    \
{                                                    \
        clock_gettime(CLOCK_MONOTONIC, &_now);                   \
        if ((_now.tv_nsec - _st.tv_nsec)<0) {            \
                _re.tv_sec  = _now.tv_sec - _st.tv_sec - 1;  \
                _re.tv_nsec = 1000000000 + _now.tv_nsec - _st.tv_nsec; \
        } else {                                         \
                _re.tv_sec  = _now.tv_sec - _st.tv_sec;      \
                _re.tv_nsec = _now.tv_nsec - _st.tv_nsec;    \
        }                                                \
}

/*
 * initializes replication
 * - by starting listener to accept mgmt connections
 * - reads data on accepted mgmt connection
 */
void *
init_replication(void *arg __attribute__((__unused__))) {
	struct epoll_event event, *events;
	int rc, sfd, event_count, i;
	int64_t epfd;
	replica_t *replica, *r;
	int timeout;
	struct timespec last, now, diff;
	mgmt_event_t *mevent;

	//Create a listener for management connections from replica
	const char* externalIP = getenv("externalIP");
	if((sfd = cstor_ops.conn_listen(externalIP, 6060, 32)) < 0) {
		REPLICA_LOG("conn_listen() failed, errorno:%d sfd:%d", errno, sfd);
		exit(EXIT_FAILURE);
	}
	epfd = epoll_create1(0);
	event.data.fd = sfd;
	event.events = EPOLLIN | EPOLLET | EPOLLERR | EPOLLHUP;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &event);
	if (rc == -1) {
		REPLICA_ERRLOG("epoll_ctl() failed, errrno:%d", errno);
		exit(EXIT_FAILURE);
	}
	events = calloc(MAXEVENTS, sizeof(event));
	timeout = 60 * 1000;	// 60 seconds
	clock_gettime(CLOCK_MONOTONIC, &last);

	while (1) {
		//Wait for management connections(on sfd) and management commands(on mgmt_rfds[]) from replicas
		event_count = epoll_wait(epfd, events, MAXEVENTS, timeout);
		if (event_count < 0) {
			if (errno == EINTR)
				continue;
			REPLICA_ERRLOG("epoll_wait ret %d err %d.. better to restart listener\n", event_count, errno);
			continue;
		}

		for(i=0; i< event_count; i++) {
			if (events[i].events & EPOLLHUP || events[i].events & EPOLLERR ||
				events[i].events & EPOLLRDHUP) {
				if (events[i].data.fd == sfd) {
					//TODO: should we do exit here?
					REPLICA_ERRLOG("epoll event %d on fd %d... better to restart listener\n",
					    events[i].events, events[i].data.fd);
				} else {
					REPLICA_ERRLOG("epoll event %d on replica\n",
					    events[i].events);
					replica = (replica_t *)events[i].data.ptr;
					//TODO: refcount to handle remove path from replica_receiver
					close(replica->mgmt_fd);
					remove_replica_from_list(replica->spec, replica->iofd);
				}
			} else {
				if (events[i].data.fd == sfd) {
					//Accept management connections from replicas and add the replicas to replica queue
					accept_mgmt_conns(epfd, sfd);
				} else {
					mevent = events[i].data.ptr;
					r = mevent->r_ptr;

					rc = 0;
					if (events[i].events & EPOLLIN) {
						if (mevent->fd == r->mgmt_fd)
							rc = handle_read_data_event(r);
						else
							rc = handle_mgmt_event_fd(r);
					}
					if (rc == -1)
						handle_mgmt_conn_error(r);

					rc = 0;
					if (events[i].events & EPOLLOUT)
						//ASSERT(mevent->fd == r->mgmt_fd);
						rc = handle_write_data_event(r);
					if (rc == -1)
						handle_mgmt_conn_error(r);
				}
			}
		}

		// send replica_status query to degraded replicas at interval of 60 seconds
		timesdiff(last, now, diff);
		if (diff.tv_sec >= 60) {
			spec_t *spec = NULL;
			MTX_LOCK(&specq_mtx);
			TAILQ_FOREACH(spec, &spec_q, spec_next) {
				ask_replica_status_all(spec);
			}
			MTX_UNLOCK(&specq_mtx);
			clock_gettime(CLOCK_MONOTONIC, &last);
		}
	}

	free (events);
	close (sfd);
	close (epfd);
	return EXIT_SUCCESS;
}
/*
int
remove_volume(spec_t *spec) {

	rcommon_cmd_t *cmd, *next_cmd = NULL;
	rcmd_t *rcmd, *next_rcmd = NULL;

	//Remove all cmds from rwaitq and rblockedq of all replicas
	TAILQ_FOREACH(replica, &spec->rq, r_next) {
		if(replica == NULL) {
			perror("Replica not present");
			exit(EXIT_FAILURE);
		}
		MTX_LOCK(&replica->q_mtx);
		rcmd = TAILQ_FIRST(&replica->rwaitq);
		while (rcmd) {
			rcmd->status = -1;
			next_rcmd = TAILQ_NEXT(rcmd, rwait_cmd_next);
			TAILQ_REMOVE(&rwaitq, rcmd, rwait_cmd_next);
			rcmd = next_rcmd;
		}

		rcmd = TAILQ_FIRST(&replica->rblockedq);
		while (rcmd) {
			rcmd->status = -1;
			next_rcmd = TAILQ_NEXT(rcmd, rblocked_cmd_next);
			TAILQ_REMOVE(&rwaitq, rcmd, rblocked_cmd_next);
			rcmd = next_rcmd;
		}
		MTX_UNLOCK(replica->q_mtx);
	}

	//Remove all cmds from rcommon_sendq, rcommon_waitq, and rcommon_pendingq
	MTX_LOCK(&spec->rcommonq_mtx);
	cmd = TAILQ_FIRST(&spec->rcommon_sendq);
	while (cmd) {
		cmd->status = -1;
		cmd->completed = 1; \
		next_cmd = TAILQ_NEXT(cmd, send_cmd_next);
		TAILQ_REMOVE(&rcommon_sendq, cmd, send_cmd_next);
		cmd = next_cmd;
	}
	cmd = TAILQ_FIRST(&spec->rcommon_waitq);
	while (cmd) {
		cmd->status = -1;
		cmd->completed = 1; \
		next_cmd = TAILQ_NEXT(cmd, wait_cmd_next);
		TAILQ_REMOVE(&rcommon_waitq, cmd, wait_cmd_next);
		cmd = next_cmd;
	}
	cmd = TAILQ_FIRST(&spec->rcommon_pendingq);
	while (cmd) {
		cmd->status = -1;
		cmd->completed = 1; \
		next_cmd = TAILQ_NEXT(cmd, pending_cmd_next);
		TAILQ_REMOVE(&rcommon_pendingq, cmd, pending_cmd_next);
		cmd = next_cmd;
	}
	MTX_UNLOCK(spec->rcommonq_mtx);

	for(i=0; i<spec->luworkers; i++) {
		pthread_cond_signal(&spec->luworker_cond[i]);
	}

	pthread_mutex_destroy(&spec->rq_mtx);
	pthread_cond_destroy(&spec->rq_cond);
	MTX_LOCK(&specq_mtx);
	TAILQ_REMOVE(&spec_q, spec, spec_next);
	MTX_UNLOCK(&specq_mtx);
}
*/

int
initialize_replication() {
	//Global initializers for replication library
	int rc;
	TAILQ_INIT(&spec_q);
	rc = pthread_mutex_init(&specq_mtx, NULL);
	if (rc != 0) {
		perror("specq_init failed");
		return -1;
	}
	return 0;
}

int
initialize_volume(spec_t *spec) {
	int rc;
	pthread_t deadlist_cleanup_thread, replicator_thread, replica_receiver_thread;

	spec->io_seq = 0;
	TAILQ_INIT(&spec->rcommon_sendq);
	TAILQ_INIT(&spec->rcommon_wait_readq);
	TAILQ_INIT(&spec->rcommon_waitq);
	TAILQ_INIT(&spec->rcommon_pendingq);
	TAILQ_INIT(&spec->rq);
	TAILQ_INIT(&spec->rwaitq);

        if(init_mempool(&spec->rcommon_deadlist, rcmd_mempool_count, sizeof (rcmd_t), 0,
            "rcmd_mempool", NULL, NULL, NULL, false)) {
		return -1;
	}

	spec->replica_count = 0;
	spec->replication_factor = spec->lu->replication_factor;
	spec->consistency_factor = spec->lu->consistency_factor;
	spec->healthy_rcount = 0;
	spec->degraded_rcount = 0;
	spec->ready = false;
	spec->receiver_epfd = epoll_create1(0);
	if(spec->receiver_epfd < 0)
		return -1;
	rc = pthread_mutex_init(&spec->rcommonq_mtx, NULL); //check
	if (rc != 0) {
		perror("rq_mtx_init failed");
		return -1;
	}
	rc = pthread_cond_init(&spec->rcommonq_cond, NULL); //check
	if (rc != 0) {
		perror("rq_mtx_init failed");
		return -1;
	}
	rc = pthread_mutex_init(&spec->rq_mtx, NULL); //check
	if (rc != 0) {
		perror("rq_mtx_init failed");
		return -1;
	}
	pthread_cond_init(&spec->rq_cond, NULL); //check
	if (rc != 0) {
		perror("rq_cond_init failed");
		return -1;
	}

	rc = pthread_create(&deadlist_cleanup_thread, NULL, &cleanup_deadlist,
			(void *)spec);
	if (rc != 0) {
		ISTGT_ERRLOG("pthread_create(replicator_thread) failed\n");
		return -1;
	}
#if 0
	rc = pthread_create(&replica_receiver_thread, NULL, &replica_receiver,
			(void *)spec);
	if (rc != 0) {
		ISTGT_ERRLOG("pthread_create(replicator_thread) failed\n");
		return -1;
	}
#endif
	MTX_LOCK(&specq_mtx);
	TAILQ_INSERT_TAIL(&spec_q, spec, spec_next);
	MTX_UNLOCK(&specq_mtx);
	return 0;
}
//When all the replicas are up, make replicas as RW and change state of istgt to REAL;


int
initialize_replication_mempool(bool should_fail) {
	int rc = 0;

	rc = init_mempool(&rcmd_mempool, rcmd_mempool_count, sizeof (rcmd_t), 0,
	    "rcmd_mempool", NULL, NULL, NULL, true);
	if (rc == -1) {
		ISTGT_ERRLOG("Failed to create mempool for command\n");
		goto error;
	} else if (rc) {
		ISTGT_NOTICELOG("rcmd mempool initialized with %u entries\n",
		    rcmd_mempool.length);
		if (should_fail) {
			goto error;
		}
		rc = 0;
	}

	rc = init_mempool(&rcommon_cmd_mempool, rcommon_cmd_mempool_count,
	    sizeof (rcommon_cmd_t), 0, "rcommon_mempool", NULL, NULL, NULL, true);
	if (rc == -1) {
		ISTGT_ERRLOG("Failed to create mempool for command\n");
		goto error;
	} else if (rc) {
		ISTGT_NOTICELOG("rcmd mempool initialized with %u entries\n",
		    rcommon_cmd_mempool.length);
		if (should_fail) {
			goto error;
		}
		rc = 0;
	}

	goto exit;

error:
	if (rcmd_mempool.ring)
		destroy_mempool(&rcmd_mempool);
	if (rcommon_cmd_mempool.ring)
		destroy_mempool(&rcommon_cmd_mempool);

exit:
	return rc;
}

int
destroy_relication_mempool(void) {
	int rc = 0;

	rc = destroy_mempool(&rcmd_mempool);
	if (rc) {
		ISTGT_ERRLOG("Failed to destroy mempool for rcmd.. err(%d)\n",
		    rc);
		goto exit;
	}

	rc = destroy_mempool(&rcommon_cmd_mempool);
	if (rc) {
		ISTGT_ERRLOG("Failed to destroy mempool for rcommon_cmd.."
		    " err(%d)\n", rc);
		goto exit;
	}

exit:
	return rc;
}

static void
destroy_resp_list(rcommon_cmd_t *rcomm_cmd)
{
	int i;

	for (i = 0; i < rcomm_cmd->copies_sent; i++) {
		if (rcomm_cmd->resp_list[i].data_ptr) {
			free(rcomm_cmd->resp_list[i].data_ptr);
		}
	}
}

void *
cleanup_deadlist(void *arg)
{
	spec_t *spec = (spec_t *)arg;
	rcommon_cmd_t *rcomm_cmd;
	int i, count = 0, entry_count = 0;

	while (1) {
		entry_count = get_num_entries_from_mempool(&spec->rcommon_deadlist);
		while (entry_count) {
			count = 0;
			rcomm_cmd = get_from_mempool(&spec->rcommon_deadlist);

//			MTX_LOCK(rcomm_cmd->mutex);
			for (i = 0; i < rcomm_cmd->copies_sent; i++) {
				if (rcomm_cmd->resp_list[i].status)
					count++;
			}
//			MTX_UNLOCK(rcomm_cmd->mutex);

			if (count == rcomm_cmd->copies_sent) {
				destroy_resp_list(rcomm_cmd);
				free(rcomm_cmd->iov[0].iov_base);
				pthread_mutex_destroy(&(rcomm_cmd->rcommand_mtx));
				for (i=1; i<rcomm_cmd->iovcnt + 1; i++)
					xfree(rcomm_cmd->iov[i].iov_base);
				REPLICA_ERRLOG("count mached : %d :%d\n", count, rcomm_cmd->copies_sent);
				memset(rcomm_cmd, 0, sizeof(rcommon_cmd_t));
				put_to_mempool(&rcommon_cmd_mempool, rcomm_cmd);
			} else {
				REPLICA_ERRLOG("rc : %p, count:%d, cc:%d\n", rcomm_cmd, count, rcomm_cmd->copies_sent);
				put_to_mempool(&spec->rcommon_deadlist, rcomm_cmd);
			}
			entry_count--;
		}
		sleep(1);	//add predefined time here
	}
	return (NULL);
}
