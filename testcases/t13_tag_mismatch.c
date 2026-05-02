/* Test 13: Tag mismatch — sender uses tag 42, receiver waits for tag 99
 * BUG: message will never match → program hangs */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    float data = 3.14f;

    if (rank == 0) {
        /* BUG: sending with tag 42 */
        MPI_Send(&data, 1, MPI_FLOAT, 1, 42, MPI_COMM_WORLD);
    } else if (rank == 1) {
        /* BUG: expecting tag 99 — will never match tag 42 → hang */
        /* Use MPI_ANY_TAG to demonstrate the sanitizer catch */
        MPI_Recv(&data, 1, MPI_FLOAT, 0, 99, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("[Rank 1] data=%f\n", data);
    }

    MPI_Finalize();
    return 0;
}