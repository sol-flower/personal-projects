/*
    Parallel Quicksort using MPI cluster on a hypercube topology & comparison with best sequential sorting algorithm.

    Compile and run with:
        mpicc quicksort_hypercube.c -o q2 -lm
        mpirun --oversubscribe -np 4 ./q2 5 2 8 1 9 3 7 4 6 0 15 12 11 77 54 22 78 43 99 90 98 54 23 34

    ** In order to run this file, a connection to an MPI environment is required. **
*/

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Comparison function for qsort
int compare(const void *a, const void *b)
{
    return (*(int *)a - *(int *)b);
}

int* hypercube_quicksort(int *local_data, int *local_size, int dimension, MPI_Comm comm)
{
    int rank, size;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &size);

    int *dims = malloc(dimension * sizeof(int));
    for (int i = 0; i < dimension; i++) {
        dims[i] = 2; // Hypercube has size 2 in each dimension
    }

    int *periods = malloc(dimension * sizeof(int));
    for (int i = 0; i < dimension; i++) {
        periods[i] = 1; // Periodic for proper hypercube behavior
    }

    MPI_Comm hypercube_comm;
    MPI_Cart_create(comm, dimension, dims, periods, 0, &hypercube_comm);

    int hypercube_rank;
    MPI_Comm_rank(hypercube_comm, &hypercube_rank);

    int *coords = malloc(dimension * sizeof(int));
    MPI_Cart_coords(hypercube_comm, hypercube_rank, dimension, coords); // Coordinates of current process

    int *result_data = malloc(*local_size * sizeof(int));   //array that stores the final sorted data
    memcpy(result_data, local_data, *local_size * sizeof(int));
    int result_size = *local_size;

    for (int i = 0; i < dimension; i++)
    {
        int local_sample = (result_size > 0) ? result_data[0] : 0;  // Pivot selection between first elements of each process
        int *all_samples = NULL;
        
        if (rank == 0) {
            all_samples = malloc(size * sizeof(int));
        }
        
        MPI_Gather(&local_sample, 1, MPI_INT, all_samples, 1, MPI_INT, 0, comm);
        
        int pivot;
        if (rank == 0) {
            qsort(all_samples, size, sizeof(int), compare); // Simple pivot: use median of first elements
            pivot = all_samples[size / 2];
            free(all_samples);
        }

        MPI_Bcast(&pivot, 1, MPI_INT, 0, comm); // Broadcast pivot from rank 0

        int smallNumCnt = 0; // Count elements for partitions
        for (int j = 0; j < result_size; j++)
        {
            if (result_data[j] <= pivot)
                smallNumCnt++;
        }
        int largeNumCnt = result_size - smallNumCnt;

        int *small_arr = malloc(smallNumCnt * sizeof(int)); // Create partitions arrays
        int *larger_arr = malloc(largeNumCnt * sizeof(int));

        int sIndex = 0, lIndex = 0; // Fill partitions
        for (int j = 0; j < result_size; j++)
        {
            if (result_data[j] <= pivot)
                small_arr[sIndex++] = result_data[j];
            else
                larger_arr[lIndex++] = result_data[j];
        }

        // Shift along dimension i to find partner
        int source, dest;
        MPI_Cart_shift(hypercube_comm, i, 1, &source, &dest);
        int bit = coords[i];

        int send_size, recv_size, *send_buf, *recv_buf; // Define send and receive buffers

        if (bit == 0)
        {
            send_size = largeNumCnt;
            send_buf = larger_arr;
        }
        else
        {
            send_size = smallNumCnt;
            send_buf = small_arr;
        }
        recv_buf = malloc(sizeof(int) * (*local_size * 2)); // Safe buffer size

        // Exchange sizes first
        MPI_Sendrecv(&send_size, 1, MPI_INT, dest, 0, &recv_size, 1, MPI_INT, source, 0, hypercube_comm, MPI_STATUS_IGNORE);

        // Exchange data
        MPI_Sendrecv(send_buf, send_size, MPI_INT, dest, 1, recv_buf, recv_size, MPI_INT, source, 1, hypercube_comm, MPI_STATUS_IGNORE);

        // Rebuild result data
        free(result_data);
        if (bit == 0)
        {
            result_size = smallNumCnt + recv_size;
            result_data = malloc(result_size * sizeof(int));
            memcpy(result_data, small_arr, smallNumCnt * sizeof(int));
            memcpy(result_data + smallNumCnt, recv_buf, recv_size * sizeof(int));
        }
        else
        {
            result_size = largeNumCnt + recv_size;
            result_data = malloc(result_size * sizeof(int));
            memcpy(result_data, larger_arr, largeNumCnt * sizeof(int));
            memcpy(result_data + largeNumCnt, recv_buf, recv_size * sizeof(int));
        }

        free(small_arr);
        free(larger_arr);
        free(recv_buf);
        MPI_Barrier(hypercube_comm);
    }

    // Final local sort
    qsort(result_data, result_size, sizeof(int), compare);
    *local_size = result_size;

    free(dims);
    free(periods);
    free(coords);
    MPI_Comm_free(&hypercube_comm);

    return result_data;
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int power_of_two_check = (size & (size - 1)) == 0; // Checker to ensure power of 2 processors
    if (!power_of_two_check) 
    { 
        if (rank == 0)
            printf("Usage: mpirun -np <power_of_2> %s <elements>\n", argv[0]);
        MPI_Finalize();
        return 1;
    }

    int n = argc - 1; // Take all CLI args without program name

    if (n % size != 0)
    { // Check if n is divisible by number of processes
        if (rank == 0)
        {
            printf("Error: Number of elements (%d) must be divisible by number of processes (%d)\n", n, size);
        }
        MPI_Finalize();
        return 1;
    }

    int local_size = n / size;
    int *local_data = malloc(local_size * sizeof(int));

    int *global_data = NULL; // Process 0 reads all elements from command line and scatters them
    if (rank == 0)
    {
        global_data = malloc(n * sizeof(int));
        printf("Input array is made of %d elements: ", n);
        for (int i = 0; i < n; i++)
        {
            global_data[i] = atoi(argv[i + 1]);
            printf("%d ", global_data[i]);
        }
        printf("\n");
    }

    // Scatter the data to all processes
    MPI_Scatter(global_data, local_size, MPI_INT, local_data, local_size, MPI_INT, 0, MPI_COMM_WORLD);

    // Calculate dimension dynamically
    int dimension = (int)log2(size);

    if (rank == 0) {
        printf("Using %d-dimensional hypercube with %d processes\n", dimension, size);
    }

    double start = MPI_Wtime();
    int *sorted_data = hypercube_quicksort(local_data, &local_size, dimension, MPI_COMM_WORLD);
    double end = MPI_Wtime();

    free(local_data);

    if (rank == 0)
    {
        printf("Parallel algo time: sorted %d elements in %.6f seconds\n", n, end - start);

        printf("Now performing sequential quicksort on given data...\n");   //calculate sequential quicksort time for comparison
        double seq_start = MPI_Wtime();
        qsort(global_data, n, sizeof(int), compare);
        double seq_end = MPI_Wtime();
        printf("Sequential algo time: sorted %d elements in %.6f seconds\n", n, seq_end - seq_start);

        free(global_data);
    }

    free(sorted_data);
    MPI_Finalize();
    return 0;
}