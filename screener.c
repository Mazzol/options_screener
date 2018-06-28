#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#include <sys/types.h>

#include "screener.h"
#include "general_stocks.h"
#include "options.h"
#include "safe.h"

long pl_size;
struct historical_price **price_list;

int main(int argc, char *argv[])
{
	int fd, mode, cont, status, ta_size, saved_stdout;
	long parent_array_size;
	pid_t pid;
	char **tick_array = NULL;
	char max_price[10], min_weight[6], skip_option[10], write_to_file[10], *newname;
	struct parent_stock **parent_array;

	mode = REGULAR;
	cont = TRUE;
	ta_size = 0;

	printf("Skip fetching webpages? ");
	fgets(skip_option, 10, stdin);

	if (strstr(skip_option, "Y") || strstr(skip_option, "y"))
		mode = REGULAR;
	else
	{
		if ((pid = fork()) == 0) // if child, exec python program
		{
			printf("Collecting stock and option data...\n");
			execlp("python3", "python3", "options_collector.py", (char *)NULL);

			printf("Warning: Unable to gather data\n");
			exit(EXIT_FAILURE); // if it reaches this point, the exec failed and the program should quit running
		}

		waitpid(pid, &status, 0);
		if (status)
		{
			printf("Warning: Unable to gather data\n");
			exit(EXIT_FAILURE);
		}
	}

	if (mode == REGULAR)
	{
		printf("Gathering historical stock prices from database...\n");
		parent_array = gather_tickers(&parent_array_size); // collects all historical data and stores in structs

		gather_options(parent_array, parent_array_size);												 // collects all data from database and stores in structs
		screen_volume_oi_baspread(parent_array, parent_array_size);									 // screens for volume/oi requirements, bid x ask spread
		calc_basic_data(parent_array, parent_array_size, atof(max_price), atof(min_weight)); // calculates weights, etc.

		// should probably break it up such that you gather all the data and then have one function called calc_weights that will
		// be called so that you can easily adjust how things are weighted rather than having to go through the code and trying to
		// find the random spots where the weights are assigned

		// perhaps even implement more abstraction such that you will have one class that just collects data from the database, and then one class that
		// just calculates data from those data points and determines the weights

		// printing all data

		// printing largest volumes of the day

		// print_large_volumes(parent_array, parent_array_size);

		while (TRUE)
		{
			printf("Maximum option price ('q' to quit): ");
			fgets(max_price, 10, stdin);
			if (strstr(max_price, "q") || strstr(max_price, "Q"))
				break;

			printf("Minimum weight: ");
			fgets(min_weight, 6, stdin);
			if (strstr(min_weight, "q") || strstr(min_weight, "Q"))
				break;

			fd = STDOUT_FILENO;
			print_data(parent_array, parent_array_size, atof(max_price), atof(min_weight));

			printf("Write to text file (Y filename)? ");
			fgets(write_to_file, 100, stdin);

			if (strstr(write_to_file, "y") || strstr(write_to_file, "Y"))
			{
				newname = strstr(write_to_file, " ") + 1;
				fd = open(newname, O_CREAT | O_WRONLY, 0666);

				saved_stdout = dup(STDOUT_FILENO);
				dup2(fd, STDOUT_FILENO);

				print_data(parent_array, parent_array_size, atof(max_price), atof(min_weight));
				dup2(saved_stdout, STDOUT_FILENO);
			}
		}

		free_parent_array(parent_array, parent_array_size);
	}

	free_tick_array(tick_array, ta_size);

	return 0;
}

/* parses argv, decides which mode to use, creates list containing personalized stocks, if necessary */
char **parse_args(int argc, char *argv[], int *mode, int *ta_size)
{
	int i;
	char **tick_array = safe_malloc(sizeof(char *));

	i = 2;
	*ta_size = 0;

	if ((argc > 1) && (argv[1][0] == '-')) // if there is a flag
	{
		switch (argv[1][1]) // determine which flag they inputted
		{
		case 'o': // if they want to only use input stocks
					 // fall-through
			*mode = NEW_STOCKS;
		case 'a': // if they want to append stocks
			if (*mode != NEW_STOCKS)
				*mode = APPEND_STOCKS;

			for (; i < argc; i++) // collect all desired tickers
			{
				tick_array = safe_realloc(tick_array, ++(*ta_size) * sizeof(char *));
				tick_array[*ta_size - 1] = safe_malloc(sizeof(char));

				strcpy(tick_array[*ta_size - 1], argv[i]);
			}

			return tick_array;
		default: // if there is a usage error
			fprintf(stderr, "usage: ./screener [ -oa ] [ tickers ]\n");
			exit(EXIT_FAILURE);
		}
	}

	*mode = REGULAR;
	return NULL;
}

void print_data(struct parent_stock **parent_array, int parent_array_size, float max_option_price, float min_weight)
{
	int printed, outter_i, inner_i;
	float weight;

	printf("TYPE\t  TICK\t  STRIKE\t DTE\t  BID\t  ASK\tWEIGHT\n");
	printf("----------------------------------------------------------------------------------\n");

	for (outter_i = 0; outter_i < parent_array_size; outter_i++)
	{
		printed = FALSE;

		for (inner_i = 0; inner_i < parent_array[outter_i]->calls_size; inner_i++)
		{
			if (parent_array[outter_i]->calls[inner_i] != NULL)
			{
				weight = parent_array[outter_i]->calls[inner_i]->weight;
				weight += parent_array[outter_i]->calls[inner_i]->parent->weight;
				weight += parent_array[outter_i]->calls_weight;

				if (weight > min_weight && parent_array[outter_i]->calls[inner_i]->bid < max_option_price)
				{
					printf("Call\t%6s\t%8f\t%4d\t%5f\t%5f\t%f\n", parent_array[outter_i]->calls[inner_i]->ticker,
							 parent_array[outter_i]->calls[inner_i]->strike, parent_array[outter_i]->calls[inner_i]->days_til_expiration,
							 parent_array[outter_i]->calls[inner_i]->bid, parent_array[outter_i]->calls[inner_i]->ask, parent_array[outter_i]->calls[inner_i]->weight);
				}
			}
		}

		for (inner_i = 0; inner_i < parent_array[outter_i]->puts_size; inner_i++)
		{
			if (parent_array[outter_i]->puts[inner_i] != NULL)
			{
				weight = parent_array[outter_i]->puts[inner_i]->weight;
				weight += parent_array[outter_i]->puts[inner_i]->parent->weight;
				weight += parent_array[outter_i]->puts_weight;

				if (weight > min_weight && parent_array[outter_i]->puts[inner_i]->bid < max_option_price)
				{
					printf("Put \t%6s\t%8f\t%4d\t%5f\t%5f\t%f\n", parent_array[outter_i]->puts[inner_i]->ticker,
							 parent_array[outter_i]->puts[inner_i]->strike, parent_array[outter_i]->puts[inner_i]->days_til_expiration,
							 parent_array[outter_i]->puts[inner_i]->bid, parent_array[outter_i]->puts[inner_i]->ask, parent_array[outter_i]->puts[inner_i]->weight);
				}
			}
		}

		if (printed == TRUE)
			printf("\n");
	}

	return;
}

/* frees all mallocs made in main, or functions called by main */
void free_tick_array(char **tick_array, int ta_size)
{
	int i;

	for (i = 0; i < ta_size; i++)
		free(tick_array[i]);

	return;
}

void free_parent_array(struct parent_stock **parent_array, int parent_array_size)
{
	int outter_i, inner_i;

	for (outter_i = 0; outter_i < parent_array_size; outter_i++)
	{
		for (inner_i = 0; inner_i < parent_array[outter_i]->calls_size; inner_i++)
		{
			if (parent_array[outter_i]->calls[inner_i] != NULL)
				free(parent_array[outter_i]->calls[inner_i]);
		}

		for (inner_i = 0; inner_i < parent_array[outter_i]->puts_size; inner_i++)
		{
			if (parent_array[outter_i]->puts[inner_i] != NULL)
				free(parent_array[outter_i]->puts[inner_i]);
		}

		free(parent_array[outter_i]);
	}

	return;
}

void print_large_volumes(struct parent_stock **parent_array, int parent_array_size)
{
	int count, min_vol, outter_i, inner_i, min_vol_index;
	struct option *largest_volumes[MIN_VOL_LENGTH];

	count = 0;
	min_vol = 0;
	min_vol_index = 0;

	for (outter_i = 0; outter_i < parent_array_size; outter_i++)
	{
		for (inner_i = 0; inner_i < parent_array[outter_i]->calls_size; inner_i++)
		{
			if (parent_array[outter_i]->calls[inner_i] != NULL)
			{
				count++;

				if (count < MIN_VOL_LENGTH)
				{
					copy_option(largest_volumes[count], parent_array[outter_i]->calls[inner_i]);
					if (parent_array[outter_i]->calls[inner_i]->volume > min_vol)
					{
						min_vol = parent_array[outter_i]->calls[inner_i]->volume;
						min_vol_index = count;
					}
				}
				else if (parent_array[outter_i]->calls[inner_i]->volume > min_vol)
				{
					copy_option(largest_volumes[min_vol_index], parent_array[outter_i]->calls[inner_i]);
					find_min_vol(largest_volumes, &min_vol, &min_vol_index);
				}
			}
		}

		for (inner_i = 0; inner_i < parent_array[outter_i]->puts_size; inner_i++)
		{
			if (parent_array[outter_i]->puts[inner_i] != NULL)
			{
				count++;

				if (count < MIN_VOL_LENGTH)
				{
					copy_option(largest_volumes[count], parent_array[outter_i]->puts[inner_i]);
					if (parent_array[outter_i]->puts[inner_i]->volume > min_vol)
					{
						min_vol = parent_array[outter_i]->puts[inner_i]->volume;
						min_vol_index = count;
					}
				}
				else if (parent_array[outter_i]->puts[inner_i]->volume > min_vol)
				{
					copy_option(largest_volumes[min_vol_index], parent_array[outter_i]->puts[inner_i]);
					find_min_vol(largest_volumes, &min_vol, &min_vol_index);
				}
			}
		}
	}

	return;
}

void find_min_vol(struct option **largest_volumes, int *min_vol, int *min_vol_index)
{
	int i;

	*min_vol = INT32_MAX;

	for (i = 0; i < MIN_VOL_LENGTH; i++)
	{
		if (largest_volumes[i]->volume < *min_vol)
		{
			*min_vol = largest_volumes[i]->volume;
			*min_vol_index = i;
		}
	}

	return;
}
