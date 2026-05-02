/*
 * heat1d_clean.c — 1D heat equation stencil (correct version)
 * Should produce ZERO errors from MPI sanitizer.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define NX_LOCAL 64
#define NSTEPS   10
#define ALPHA    0.25

static void init_grid(double *u, int nx, int rank) {
    for (int i = 0; i <= nx + 1; i++) u[i] = 0.0;
    if (rank == 0) u[nx / 2] = 1.0;
}

static void apply_stencil(double *u, double *u_new, int nx) {
    for (int i = 1; i <= nx; i++)
        u_new[i] = u[i] + ALPHA * (u[i-1] - 2.0*u[i] + u[i+1]);
}

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int nx = NX_LOCAL;
    double *u     = (double*)calloc(nx + 2, sizeof(double));
    double *u_new = (double*)calloc(nx + 2, sizeof(double));

    init_grid(u, nx, rank);

    int left  = (rank > 0)        ? rank - 1 : MPI_PROC_NULL;
    int right = (rank < size - 1) ? rank + 1 : MPI_PROC_NULL;

    for (int step = 0; step < NSTEPS; step++) {
        MPI_Request reqs[4];
        int nreqs = 0;

        // Correct: use MPI_DOUBLE consistently
        if (left  != MPI_PROC_NULL)
            MPI_Irecv(&u[0],    1, MPI_DOUBLE, left,  0, MPI_COMM_WORLD, &reqs[nreqs++]);
        if (right != MPI_PROC_NULL)
            MPI_Irecv(&u[nx+1], 1, MPI_DOUBLE, right, 1, MPI_COMM_WORLD, &reqs[nreqs++]);
        if (left != MPI_PROC_NULL)
            MPI_Isend(&u[1],  1, MPI_DOUBLE, left,  1, MPI_COMM_WORLD, &reqs[nreqs++]);
        if (right != MPI_PROC_NULL)
            MPI_Isend(&u[nx], 1, MPI_DOUBLE, right, 0, MPI_COMM_WORLD, &reqs[nreqs++]);

        MPI_Waitall(nreqs, reqs, MPI_STATUSES_IGNORE);
        apply_stencil(u, u_new, nx);
        memcpy(u + 1, u_new + 1, nx * sizeof(double));
    }

    double local_max = 0.0;
    for (int i = 1; i <= nx; i++)
        if (fabs(u[i]) > local_max) local_max = fabs(u[i]);

    double global_max;
    MPI_Reduce(&local_max, &global_max, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    if (rank == 0)
        printf("[heat1d_clean] global_max = %.6f after %d steps\n", global_max, NSTEPS);

    free(u); free(u_new);
    MPI_Finalize();
    return 0;
}