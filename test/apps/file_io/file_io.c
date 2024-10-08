// SPDX-License-Identifier: MPL-2.0
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/file.h>
#include <stdint.h>

#define KB 1024
#define MB (1024 * KB)
#define GB (1024 * MB)
#define BUFFER_SIZE (4 * KB)
#define FILE_SIZE (256 * MB)
#define NUM_OF_CALLS 100000
#define BLOCK_SIZE 0x1000

#ifndef O_DIRECT
#define O_DIRECT 040000
#endif

// Function prototypes
int fill_file(int fd);
long calc_duration(struct timespec *start, struct timespec *end);
int perform_sequential_io(int fd, ssize_t (*io_func)(int, void *, size_t),
			  const char *op_name);
int perform_random_io(int fd, ssize_t (*io_func)(int, void *, size_t, off_t),
		      const char *op_name);
int sequential_read(int fd);
int sequential_write(int fd);
int random_read(int fd);
int random_write(int fd);

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <file_name>\n", argv[0]);
		return -1;
	}

	int fd = open(argv[1], O_RDWR | O_CREAT | O_DIRECT, 00666);
	if (fd == -1) {
		fprintf(stderr,
			"Failed to open the file: %s. Error message: %s.\n",
			argv[1], strerror(errno));
		return -1;
	}

	// Bugfix: multiple ftruncate failed
	if (ftruncate(fd, FILE_SIZE) < 0) {
		fprintf(stderr,
			"Failed to truncate the file: %s to size: %dMB. Error message: %s.\n",
			argv[1], FILE_SIZE / MB, strerror(errno));
		close(fd);
		return -1;
	}

	// Warm up by filling the file.
	printf("Prepare the file for the benchmarks...\n");
	if (fill_file(fd) < 0) {
		fprintf(stderr, "Failed to fill the file: %s.\n", argv[1]);
		close(fd);
		return -1;
	}

	printf("Executing the sequential read benchmark...\n");
	if (sequential_read(fd) < 0) {
		fprintf(stderr,
			"Failed to do sequential read on the file: %s.\n",
			argv[1]);
		close(fd);
		return -1;
	}

	printf("Executing the sequential write benchmark...\n");
	if (sequential_write(fd) < 0) {
		fprintf(stderr,
			"Failed to do sequential write on the file: %s.\n",
			argv[1]);
		close(fd);
		return -1;
	}

	printf("Executing the random read benchmark...\n");
	if (random_read(fd) < 0) {
		fprintf(stderr, "Failed to do random read on the file: %s.\n",
			argv[1]);
		close(fd);
		return -1;
	}

	printf("Executing the random write benchmark...\n");
	if (random_write(fd) < 0) {
		fprintf(stderr, "Failed to do random write on the file: %s.\n",
			argv[1]);
		close(fd);
		return -1;
	}

	close(fd);

	if (unlink(argv[1]) < 0) {
		fprintf(stderr,
			"Failed to delete the file: %s. Error message: %s.\n",
			argv[1], strerror(errno));
		return -1;
	}

	return 0;
}

int fill_file(int fd)
{
	ssize_t bytes_write, offset = 0;
	char *buffer;

	if (posix_memalign((void **)&buffer, BLOCK_SIZE, BUFFER_SIZE) != 0) {
		fprintf(stderr, "Failed to allocate memory.\n");
		return -1;
	}

	memset(buffer, 0, BUFFER_SIZE);
	lseek(fd, 0, SEEK_SET);

	while (offset < FILE_SIZE) {
		bytes_write = write(fd, buffer, BUFFER_SIZE);
		if (bytes_write == -1) {
			fprintf(stderr,
				"Failed to write the file. Error message: %s.\n",
				strerror(errno));
			free(buffer);
			return -1;
		}
		offset += bytes_write;
	}

	free(buffer);
	return 0;
}

long calc_duration(struct timespec *start, struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1e9 +
	       (end->tv_nsec - start->tv_nsec);
}

int perform_sequential_io(int fd, ssize_t (*io_func)(int, void *, size_t),
			  const char *op_name)
{
	struct timespec start, end;
	long total_nanoseconds = 0, avg_latency;
	double throughput;
	ssize_t ret_bytes, offset = 0;
	char *buffer;

	if (posix_memalign((void **)&buffer, BLOCK_SIZE, BUFFER_SIZE) != 0) {
		fprintf(stderr, "Failed to allocate memory.\n");
		return -1;
	}

	memset(buffer, 0, BUFFER_SIZE);
	lseek(fd, 0, SEEK_SET);
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (int i = 0; i < NUM_OF_CALLS; i++) {
		if (offset >= FILE_SIZE) {
			offset = lseek(fd, 0, SEEK_SET);
			if (offset == -1) {
				fprintf(stderr,
					"Failed to seek the file. Error message: %s.\n",
					strerror(errno));
				free(buffer);
				return -1;
			}
		}

		ret_bytes = io_func(fd, buffer, BUFFER_SIZE);
		if (ret_bytes == -1) {
			fprintf(stderr,
				"Failed to %s the file. Error message: %s.\n",
				op_name, strerror(errno));
			free(buffer);
			return -1;
		}

		offset += ret_bytes;
	}

	if (fsync(fd) == -1) {
		fprintf(stderr,
			"Failed to sync the file in the end. Error message: %s.\n",
			strerror(errno));
		free(buffer);
		return -1;
	}

	clock_gettime(CLOCK_MONOTONIC, &end);
	total_nanoseconds += calc_duration(&start, &end);
	avg_latency = total_nanoseconds / NUM_OF_CALLS;
	throughput = ((double)BUFFER_SIZE * NUM_OF_CALLS) /
		     ((double)total_nanoseconds / 1e9);

	printf("Executed the sequential %s (buffer size: %dKB, file size: %dMB) syscall %d times.\n",
	       op_name, BUFFER_SIZE / KB, FILE_SIZE / MB, NUM_OF_CALLS);
	printf("Syscall average latency: %ld nanoseconds, throughput: %.2f MB/s\n",
	       avg_latency, throughput / MB);

	free(buffer);
	return 0;
}

int perform_random_io(int fd, ssize_t (*io_func)(int, void *, size_t, off_t),
		      const char *op_name)
{
	struct timespec start, end;
	long total_nanoseconds = 0, avg_latency;
	double throughput;
	ssize_t ret_bytes;
	char *buffer;

	if (posix_memalign((void **)&buffer, BLOCK_SIZE, BUFFER_SIZE) != 0) {
		fprintf(stderr, "Failed to allocate memory.\n");
		return -1;
	}

	memset(buffer, 0, BUFFER_SIZE);
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (int i = 0; i < NUM_OF_CALLS; i++) {
		off_t random_offset =
			(random() % (FILE_SIZE / BUFFER_SIZE)) * BUFFER_SIZE;
		ret_bytes = io_func(fd, buffer, BUFFER_SIZE, random_offset);
		if (ret_bytes == -1) {
			fprintf(stderr,
				"Failed to %s the file. Error message: %s.\n",
				op_name, strerror(errno));
			free(buffer);
			return -1;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &end);
	total_nanoseconds += calc_duration(&start, &end);
	avg_latency = total_nanoseconds / NUM_OF_CALLS;
	throughput = ((double)BUFFER_SIZE * NUM_OF_CALLS) /
		     ((double)total_nanoseconds / 1e9);

	printf("Executed the random %s (buffer size: %dKB, file size: %dMB) syscall %d times.\n",
	       op_name, BUFFER_SIZE / KB, FILE_SIZE / MB, NUM_OF_CALLS);
	printf("Syscall average latency: %ld nanoseconds, throughput: %.2f MB/s\n",
	       avg_latency, throughput / MB);

	free(buffer);
	return 0;
}

int sequential_read(int fd)
{
	return perform_sequential_io(fd, read, "read");
}

int sequential_write(int fd)
{
	return perform_sequential_io(fd, write, "write");
}

int random_read(int fd)
{
	return perform_random_io(fd, pread, "pread");
}

int random_write(int fd)
{
	return perform_random_io(fd, pwrite, "pwrite");
}
