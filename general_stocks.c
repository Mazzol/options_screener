#include <math.h>
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

void gather_options(struct ParentStock **parent_array, long parent_array_size) {
	int i, parent_array_index;
	char previous[TICK_SIZE];

	parent_array_index = 0;
	memset(previous, 0, TICK_SIZE);

	gather_options_data();

	// iterate through all_options array
	for (i = 0; i < all_options_size; i++) {
		// if the current and previous ticker are different, means change in ticker
		if (0 != strcmp(all_options[i]->ticker, previous)) {
			// to avoid skipping the first index
			if (i != 0)
				parent_array_index++;

			memset(previous, 0, TICK_SIZE);
			strcpy(previous, all_options[i]->ticker);

			parent_array[parent_array_index]->calls = safe_malloc(sizeof(struct option *));
			parent_array[parent_array_index]->puts = safe_malloc(sizeof(struct option *));
			parent_array[parent_array_index]->calls_size = 0;
			parent_array[parent_array_index]->puts_size = 0;
			parent_array[parent_array_index]->weight = 0;
		}

		// if it's a call option
		if (all_options[i]->type == 1) {
			parent_array[parent_array_index]->calls = safe_realloc(parent_array[parent_array_index]->calls,
																					 ++(parent_array[parent_array_index]->calls_size) * sizeof(struct option *));

			parent_array[parent_array_index]->calls[parent_array[parent_array_index]->calls_size - 1] = safe_malloc(sizeof(struct option));
			parent_array[parent_array_index]->calls[parent_array[parent_array_index]->calls_size - 1]->parent = parent_array[parent_array_index];
			copy_option(parent_array[parent_array_index]->calls[parent_array[parent_array_index]->calls_size - 1], all_options[i]);
		}
		else if (all_options[i]->type == 0) {
			parent_array[parent_array_index]->puts = safe_realloc(parent_array[parent_array_index]->puts,
																					++(parent_array[parent_array_index]->puts_size) * sizeof(struct option *));

			parent_array[parent_array_index]->puts[parent_array[parent_array_index]->puts_size - 1] = safe_malloc(sizeof(struct option));
			parent_array[parent_array_index]->puts[parent_array[parent_array_index]->puts_size - 1]->parent = parent_array[parent_array_index];
			copy_option(parent_array[parent_array_index]->puts[parent_array[parent_array_index]->puts_size - 1], all_options[i]);
		}

		free(all_options[i]);
	}

	return;
}

/* Gathers all options data from database */
void gather_options_data(void) {
	int rc;
	char *error_msg;
	sqlite3 *db;
	char *sql = "SELECT * FROM optionsData";

	rc = sqlite3_open("optionsData", &db);

	all_options_size = 0;
	all_options = malloc(sizeof(struct option *));

	if (rc != SQLITE_OK) {
		fprintf(stderr, "Failed to fetch data: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return;
	}

	rc = sqlite3_exec(db, sql, options_callback, 0, &error_msg);
	sqlite3_close(db);

	return;
}

/* 
 * Effectively removes options from the list if the volume/open interest isn't up to standards
 * Also screens for bid x ask spread
 */
void screen_volume_oi_baspread(struct ParentStock **parent_array, int parent_array_size) {
	/*
    * ALGORITHM/REQUIREMENTS:
    * 
    * if dte <= 30, min_vol = 2000 / (dte / 2)
    * 
    * The closer to the DTE, the more open interest/volume there must be. However, there may never 
    * be any otion that has no volume (NOT OPEN INTEREST) below 10, no matter the DTE.
    * If within one month of expiration, the option must have at least 134 volume on the day
    */

	char removed;
	unsigned short dte;
	unsigned int outter_i, inner_i, volume, open_interest;
	float bid, ask, min_vol;

	for (outter_i = 0; outter_i < parent_array_size; outter_i++) {
		parent_array[outter_i]->num_open_calls = parent_array[outter_i]->calls_size;
		parent_array[outter_i]->num_open_puts = parent_array[outter_i]->puts_size;
		large_price_drop(parent_array[outter_i]);
		avg_stock_close(parent_array[outter_i]);
		perc_from_high_low(parent_array[outter_i]);

		for (inner_i = 0; inner_i < parent_array[outter_i]->calls_size; inner_i++) {
			removed = FALSE;
			bid = parent_array[outter_i]->calls[inner_i]->bid;
			ask = parent_array[outter_i]->calls[inner_i]->ask;
			volume = parent_array[outter_i]->calls[inner_i]->volume;
			open_interest = parent_array[outter_i]->calls[inner_i]->open_interest;
			dte = parent_array[outter_i]->calls[inner_i]->days_til_expiration;

			if (volume < 10 || open_interest < 100 || bid < 3 || ask < 2)
				removed = TRUE;
			if (!removed && dte < 30) {
				// honestly, this is a random equation. It basically says the closer to the dte,
				// the larger the volume must be, so if dte = 2, volume must be at least 2000
				if (dte <= 1)
					removed = TRUE;
				else
					min_vol = 2000 / (dte / 2);

				if (volume < min_vol)
					removed = TRUE;
			}
			if (!removed && bid_ask_spread(parent_array[outter_i]->calls[inner_i])) {
				removed = TRUE;
			}

			if (removed) {
				parent_array[outter_i]->calls[inner_i] = NULL;
				parent_array[outter_i]->num_open_calls--;
			}
		}

		for (inner_i = 0; inner_i < parent_array[outter_i]->puts_size; inner_i++) {
			removed = FALSE;
			volume = parent_array[outter_i]->puts[inner_i]->volume;
			open_interest = parent_array[outter_i]->puts[inner_i]->open_interest;
			dte = parent_array[outter_i]->puts[inner_i]->days_til_expiration;

			if (volume < 10 || open_interest < 100)
				removed = TRUE;
			if (!removed && dte < 30) {
				// honestly, this is a random equation. It basically says the closer to the dte,
				// the larger the volume must be, so if dte = 2, volume must be at least 2000
				if (dte <= 1)
					removed = TRUE;
				else
					min_vol = 2000 / (dte / 2);

				if (volume < min_vol)
					removed = TRUE;
			}
			if (!removed && bid_ask_spread(parent_array[outter_i]->puts[inner_i]))
				removed = TRUE;

			if (removed) {
				parent_array[outter_i]->puts[inner_i] = NULL;
				parent_array[outter_i]->num_open_calls--;
			}
		}
	}

	return;
}

/* Gathers all historical pricing data from database */
void gather_data(void) {
	int rc;
	char *error_msg;
	sqlite3 *db;
	char *sql = "SELECT * FROM historicalPrices";

	historical_array_size = 0;
	historical_price_array = malloc(sizeof(struct HistoricalPrice *));

	rc = sqlite3_open("historicalPrices", &db);

	if (rc != SQLITE_OK) {
		fprintf(stderr, "Failed to fetch data: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);

		return;
	}

	rc = sqlite3_exec(db, sql, historical_price_callback, 0, &error_msg);

	sqlite3_close(db);

	return;
}

struct ParentStock **gather_tickers(long *pa_size) {
	int i;
	char previous[TICK_SIZE];
	long prices_array_size, parent_array_size;
	struct ParentStock **parent_array; // list of all stock tickers containing lists of their historical prices

	parent_array_size = 0;
	parent_array = safe_malloc(sizeof(struct ParentStock *));
	memset(previous, 0, TICK_SIZE);

	gather_data();

	for (i = 0; i < historical_array_size; i++) {
		if (0 != strcmp(historical_price_array[i]->ticker, previous)) {
			if (i != 0)
				find_curr_stock_price(parent_array[parent_array_size - 1]);

			memset(previous, 0, TICK_SIZE);
			strcpy(previous, historical_price_array[i]->ticker);

			parent_array = safe_realloc(parent_array, ++parent_array_size * (sizeof(struct ParentStock *)));
			parent_array[parent_array_size - 1] = safe_malloc(sizeof(struct ParentStock));

			memset(parent_array[parent_array_size - 1]->ticker, 0, TICK_SIZE);
			strcpy(parent_array[parent_array_size - 1]->ticker, historical_price_array[i]->ticker);
			parent_array[parent_array_size - 1]->prices_array = safe_malloc(sizeof(struct HistoricalPrice *));
			parent_array[parent_array_size - 1]->weight = 0;
			parent_array[parent_array_size - 1]->calls_weight = 0;
			parent_array[parent_array_size - 1]->puts_weight = 0;

			prices_array_size = 0;
		}

		parent_array[parent_array_size - 1]->prices_array = safe_realloc(parent_array[parent_array_size - 1]->prices_array, ++prices_array_size * (sizeof(struct HistoricalPrice *)));
		parent_array[parent_array_size - 1]->prices_array[prices_array_size - 1] = safe_malloc(sizeof(struct HistoricalPrice));
		parent_array[parent_array_size - 1]->prices_array_size = prices_array_size;

		strcpy(parent_array[parent_array_size - 1]->prices_array[prices_array_size - 1]->ticker, historical_price_array[i]->ticker);
		parent_array[parent_array_size - 1]->prices_array[prices_array_size - 1]->date = historical_price_array[i]->date;
		parent_array[parent_array_size - 1]->prices_array[prices_array_size - 1]->open = historical_price_array[i]->open;
		parent_array[parent_array_size - 1]->prices_array[prices_array_size - 1]->low = historical_price_array[i]->low;
		parent_array[parent_array_size - 1]->prices_array[prices_array_size - 1]->high = historical_price_array[i]->high;
		parent_array[parent_array_size - 1]->prices_array[prices_array_size - 1]->close = historical_price_array[i]->close;
		parent_array[parent_array_size - 1]->prices_array[prices_array_size - 1]->volume = historical_price_array[i]->volume;

		// to free up memory since this is memory intensive
		free(historical_price_array[i]);
	}

	free(historical_price_array);

	*pa_size = parent_array_size;
	return parent_array;
}

void find_curr_stock_price(struct ParentStock *stock) {
	int size = stock->prices_array_size - 1;

	stock->curr_price = stock->prices_array[size]->close;

	return;
}

int historical_price_callback(void *NotUsed, int argc, char **argv, char **azColName) {
	historical_price_array = realloc(historical_price_array, ++historical_array_size * (sizeof(struct HistoricalPrice *)));
	historical_price_array[historical_array_size - 1] = malloc(sizeof(struct HistoricalPrice));

	memset(historical_price_array[historical_array_size - 1]->ticker, 0, 10);
	strcpy(historical_price_array[historical_array_size - 1]->ticker, argv[0]);
	historical_price_array[historical_array_size - 1]->date = atof(argv[1]);
	historical_price_array[historical_array_size - 1]->open = atof(argv[2]);
	historical_price_array[historical_array_size - 1]->low = atof(argv[3]);
	historical_price_array[historical_array_size - 1]->high = atof(argv[4]);
	historical_price_array[historical_array_size - 1]->close = atof(argv[5]);
	historical_price_array[historical_array_size - 1]->volume = atof(argv[6]);

	return 0;
}

void large_price_drop(struct ParentStock *stock) {
	int i, starting_index;
	float change, previous;

	starting_index = (stock->prices_array_size <= 100 ? 0 : stock->prices_array_size - 30);

	previous = 0;

	for (i = starting_index; i < stock->prices_array_size; i++) {
		if (i == 0 || i == starting_index) {
			previous = stock->prices_array[i]->close;
			continue;
		}

		change = (previous - stock->prices_array[i]->close) / previous;
		change = (change < 0 ? change *= -1 : change); // since abs() only works on ints

		if (change * 100 >= 7.5)
			stock->weight += 100 * (change / 7.5);
	}

	// finding total change over the period
	change = (stock->prices_array[starting_index]->close - stock->prices_array[i - 1]->close) / stock->prices_array[starting_index]->close;
	change = (change < 0 ? change *= -1 : change); // since abs() only works on ints

	if (change * 100 >= 10)
		stock->weight += 100 * (change / 10);

	return;
}

void perc_from_high_low(struct ParentStock *stock) {
	int i;
	float dif, low, high, weight;

	low = INT64_MAX;
	high = 0;

	for (i = 0; i < stock->prices_array_size; i++) {
		if (stock->prices_array[i]->close < low)
			low = stock->prices_array[i]->close;
		else if (stock->prices_array[i]->close > high)
			high = stock->prices_array[i]->close;
	}

	stock->yearly_low = low;
	stock->yearly_high = high;

	dif = low - stock->curr_price;
	dif = (dif < 0 ? dif *= -1 : dif);

	stock->perc_from_year_low = (dif / stock->curr_price) * 100;
	stock->perc_from_year_high = ((high - stock->curr_price) / stock->curr_price) * 100;

	// weight to be assigned given percent from low
	weight = 100 - (stock->perc_from_year_low * 100);
	stock->calls_weight += weight;

	// weight to be assigned given percent from high
	weight = 100 - (stock->perc_from_year_high * 100);
	stock->puts_weight += weight;

	return;
}

/*
 * General algorithm:
 * Everything will be weighted as we go, so there will be a positive and a negative weight.
 * It will be adjusted as we go, so I'll use a consecutive_days variable to determine how
 * many days in a row the same price trend has been going. It might also be smart to create
 * a more generalized version because there will likely be dips even when it is generally a
 * strong uptrend pattern.
 */
void price_trend(struct ParentStock *stock) {
	int i, positive, prev_positive, consecutive_days;
	float dif, previous, current, perc_change;
	float neg_weight = 0, pos_weight = 0;

	positive = FALSE;

	for (i = 0; i < stock->prices_array_size; i++) {
		current = stock->prices_array[i]->close;

		if (i == 0) {
			previous = current;
			continue;
		}

		dif = current - previous;
		positive = (dif > 0 ? TRUE : FALSE);

		perc_change = fabsf(dif) / previous;
		consecutive_days = (prev_positive == positive ? ++consecutive_days : 0); // if trend is broke, consecutive_days is reset

		if (positive)
			pos_weight += perc_change * consecutive_days / 5;
		else
			neg_weight += perc_change * consecutive_days / 5;

		prev_positive = positive;
	}

	stock->calls_weight += pos_weight / 7.5;
	stock->puts_weight += neg_weight / 7.5;

	return;
}

void average_perc_change(struct ParentStock *stock) {
	int i;
	float dif, change, current, previous, total_changes;

	total_changes = 0;

	for (i = 0; i < stock->prices_array_size; i++) {
		current = stock->prices_array[i]->close;

		if (i == 0) {
			previous = current;
			continue;
		}

		dif = fabsf(current - previous);
		change = dif / previous * 100;
		total_changes += change;

		previous = current;
	}

	change = total_changes / stock->prices_array_size;
	stock->weight += (change * 100);
}

void avg_stock_close(struct ParentStock *stock) {
	int i;
	float total;

	total = 0;

	for (i = 0; i < stock->prices_array_size; i++)
		total += stock->prices_array[i]->close;

	stock->avg_close = total / i;

	return;
}
