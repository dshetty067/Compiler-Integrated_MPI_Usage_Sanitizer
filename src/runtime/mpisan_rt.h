#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Called once at program start (injected before MPI_Init)
void __mpisan_init(void);

// Called just before MPI_Finalize
void __mpisan_finalize(void);

// Called before blocking MPI_Send / MPI_Recv
void __mpisan_send(void *buf, long count, long mpi_type,
                   int dest, int tag, long comm,
                   const char *file, int line, int is_blocking);

void __mpisan_recv(void *buf, long count, long mpi_type,
                   int src,  int tag, long comm,
                   const char *file, int line, int is_blocking);

// Called before non-blocking Isend / Irecv
void __mpisan_isend(void *buf, long count, long mpi_type,
                    int dest, int tag, long comm,
                    void *req_ptr, const char *file, int line);

void __mpisan_irecv(void *buf, long count, long mpi_type,
                    int src, int tag, long comm,
                    void *req_ptr, const char *file, int line);

// Called before collectives
void __mpisan_collective(const char *op, void *buf, long count, long mpi_type,
                         long comm, const char *file, int line);

// Called before MPI_Wait
void __mpisan_wait(void *req_ptr, const char *file, int line);

// Called before MPI_Waitall
void __mpisan_waitall(int count, void *req_array, const char *file, int line);

// Called before MPI_Barrier
void __mpisan_barrier(long comm, const char *file, int line);

#ifdef __cplusplus
}
#endif