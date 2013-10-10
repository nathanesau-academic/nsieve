#include "nsieve.h"

void era_sieve (nsieve_t *ns, char *vals){
	// assumes vals has been allocated and cleared with sufficient space for fb_bound chars.
	// 0 is prime, 1 is composite.
	for (int skip=2; skip < (int) (sqrt(ns->fb_bound) + 1); skip++){
		if (vals[skip-2] == 1){
			continue;
		}
		for (int pos=2*skip; pos < ns->fb_bound; pos += skip){
			vals[pos-2] = 1;
		}
	}
}

void extract (nsieve_t *ns, char *vals){
	// first count the number of primes found such that (n/p) = 1. (n/p) is the Legendre symbol.
	int count = 0;
	for (int i=0; i < ns->fb_bound-2; i++){
		if (vals[i] == 0){
			if (i == 0 || mpz_kronecker_ui(ns->N, i+2) == 1){	// we must admit 2, since N is always a QR mod 2, but there's something wierd about the kronecker symbol for 2  because it's not odd.
				count++;
			}
		}
	}
	ns->fb_len = count;
	ns->rels_needed = ns->fb_len + ns->extra_rels;
	ns->fb = (uint32_t *)(malloc(count * sizeof(uint32_t)));
	int w = 0;
	for (int i=0; i < ns->fb_bound-2; i++){
		if (vals[i] == 0 && (i == 0 || mpz_kronecker_ui(ns->N, i+2) == 1)){
			ns->fb[w] = i+2;
			w++;
		}
	}
}

/* Generates the factor base, given the bound fb_bound; will also compute the roots of N mod each p. */
void generate_fb (nsieve_t *ns){
	// first we use the sieve of Eratosthenes and the mpz_kronecker function to find a list of primes p < fb_bound such that (n/p) = 1.
	char *erasieve_vals = (char *)(calloc(ns->fb_bound, sizeof(char)));
	era_sieve (ns, erasieve_vals);
	extract (ns, erasieve_vals);
	free (erasieve_vals);

	// now we compute the modular square roots of n mod each p. 
	ns->roots = (uint32_t *)(malloc(ns->fb_len * sizeof(uint32_t)));
	ns->fb_logs = (uint8_t *)(malloc(ns->fb_len * sizeof(uint8_t)));
	for (int i=0; i<ns->fb_len; i++){
		ns->roots[i] = find_root (ns->N, ns->fb[i]);	// see common.c for the implementation of this method.
		// note that there are actually 2 square roots; however, the second may be obtained readily as p - sqrt#1, so only one is stored.
		ns->fb_logs[i] = fast_log (ns->fb[i]);
	}
}
const int PARAM_FBBOUND = 1;
const int PARAM_LPBOUND = 2;
const int PARAM_M = 3;
const int PARAM_T = 4;

#define NPLEVELS  4	// number of entries in the params table
#define  NPARAMS  5

const double params[NPLEVELS][NPARAMS] =   { 	{100,  5000, 5000,  1 * 32768, 1.3},
						{120, 11000, 11000, 2 * 32768, 1.3},
						{140, 25000, 25000, 2 * 32768, 1.3},
					    	{160, 55000, 55000, 2 * 32768, 1.3}
					   };

void set_params (nsieve_t *ns, int p1, int p2, double fac){
	// -1 indicates that the property was not manually overridden by the user via a command line argument.
	if (ns->fb_bound == -1) ns -> fb_bound = (uint32_t) (params[p1][PARAM_FBBOUND] * fac + params[p2][PARAM_FBBOUND] * (1 - fac));
	if (ns->lp_bound == 0){
		ns->lp_bound = ns->fb_bound;
	}
	if (ns->lp_bound == -1) ns -> lp_bound = (uint32_t) (params[p1][PARAM_LPBOUND] * fac + params[p2][PARAM_LPBOUND] * (1 - fac));
	if (ns->M == -1) ns -> M        = (uint32_t) (params[p1][PARAM_M] * fac + params[p2][PARAM_M] * (1 - fac));
	if (ns->T == -1) ns -> T        = (float)    (params[p1][PARAM_T] * fac + params[p2][PARAM_T] * (1 - fac));
	printf("Selected parameters: \n\tfb_bound = %d \n\tlp_bound = %d \n\tM = %d\n\tT - %f\n", ns->fb_bound, ns->lp_bound, ns->M, ns->T);
}

void select_parameters (nsieve_t *ns){
	int bits = mpz_sizeinbase (ns->N, 2);
	printf("Choosing parameters for %d bit number... \n", bits);
	if (bits <= params[0][0]){
		set_params(ns, 0, 0, 0);
	} else if (bits >= params[NPLEVELS-1][0]){
		set_params(ns, NPLEVELS - 1, NPLEVELS - 1, 0);
	} else {
		int i = 0;
		while (i < NPLEVELS && params[i][0] < bits){
			i++;
		}
		set_params (ns, i, i-1, (bits - params[i-1][0]) / (params[i][0] - params[i-1][0]));
	}
}

void select_multiplier (nsieve_t *ns){
}
	

/* Initialization and selection of the parameters for the factorization. This will fill allocate space for and initialize all of the 
 * fields in the nsieve_t. Notably, it will generate the factor base, compute the square roots, and allocate space for the matrix and partials. 
 */
void nsieve_init (nsieve_t *ns, mpz_t n){
	long start = clock();
	// all of the parameters here are complete BS for now.
	mpz_init_set (ns->N, n);

	select_parameters (ns);
	select_multiplier (ns);

	ns->nfull = 0;
	ns->npartial = 0;
	ns->tdiv_ct = 0;
	ns->sieve_locs = 0;
	ns->extra_rels = 48;

	generate_fb (ns);

	ns->relns = (matrel_t *)(malloc((ns->fb_len + ns->extra_rels) * sizeof(matrel_t)));
	ns->row_len = (ns->fb_len)/(8*sizeof(uint64_t)) + 1;	// we would need that to be ns->fb_len - 1, except we need to throw in the factor -1 into the FB. 

	printf("There are %d primes in the factor base, so we will search for %d relations. The matrix rows will have %d 8-byte chunks in them.\n", ns->fb_len, ns->rels_needed, ns->row_len);
	for (int i=0; i < ns->rels_needed; i++){
		ns->relns[i].row = (uint64_t *)(calloc(ns->row_len, sizeof(uint64_t)));
	}

	ht_init (ns);
	ns->timing.init_time = clock() - start;
}

/* Once ns has been initialized (by calling nsieve_init), this method is called to actaully perform the bulk of the factorization */
void factor (nsieve_t *ns){
	long start = clock();
	poly_gpool_t gpool;
	gpool_init (&gpool, ns);


	block_data_t sievedata;
	printf("Using k = %d; gvals range from %d to %d.\n", ns->k, gpool.gpool[0], gpool.gpool[gpool.ng-1]);
//	return;
	int poly_ct = 0;
	int pg_ct = 0;
	while (ns->nfull + ns->npartial < ns->rels_needed){
		// while we don't have enough relations, sieve another poly group.
		poly_group_t *curr_polygroup = (poly_group_t *) malloc(sizeof(poly_group_t));
		polygroup_init (curr_polygroup, ns);
		generate_polygroup (&gpool, curr_polygroup, ns);
//		printf("Starting new polygroup. We have %d full and %d partial relations\n.", ns->nfull, ns->npartial);
		for (int i = 0; i < ns -> bvals; i ++){
			poly_t *curr_poly = (poly_t *) malloc (sizeof (poly_t));
			poly_init (curr_poly);
			generate_poly (curr_poly, curr_polygroup, ns, i);
/*
			printf("\t");
			poly_print (curr_poly);
*/
			sieve_poly (&sievedata, curr_polygroup, curr_poly, ns);
//			printf("We now have %d full relations and %d partials.\nSwitching polynomials..., M = %d\n", ns->nfull, ns->npartial, curr_poly.M);
		}
		add_polygroup_relations (curr_polygroup, ns);
		ns->npartial = ht_count (&ns->partials);
		pg_ct ++;
		poly_ct = pg_ct * ns->bvals;
		printf("Have %d of %d relations (%d full + %d combined from %d partial); sieved %d polynomials from %d groups. \r", ns->nfull + ns->npartial, ns->rels_needed, ns->nfull, ns->npartial, ns->partials.nentries, poly_ct, pg_ct);
		fflush(stdout);
	}
	printf("\nSieving complete. Of %lld sieve locations, %d were trial divided. \n", ns->sieve_locs, ns->tdiv_ct);
	ns->timing.sieve_time = clock() - start;
	ns->timing.filter_time = 0;
	// now we have enough relations, so we build the matrix (combining the partials).
	
	combine_partials (ns);

	// Filter the matrix to reduce its size without reducing its yummy content. This will accelerate the matrix solving step, and also reduce the memory usage.
//	filter (ns);
	
	// now we solve the matricies. The rest of the guts are in matrix.c, in the function solve_matrix.
	solve_matrix (ns);
}

int main (int argc, const char *argv[]){
	printf ("Using GMP version %s\n", gmp_version);
	printf ("It was compiled with %s, using flags %s\n", __GMP_CC, __GMP_CFLAGS);
	nsieve_t ns;
	mpz_t n;
	mpz_init (n);

	int pos = 1;
	int nspecd = 0;

	ns.T = -1;
	ns.fb_bound = -1;
	ns.lp_bound = -1;
	ns.M = -1;
	while (pos < argc){
		if (!strcmp(argv[pos], "-T")){
			ns.T = atof (argv[pos+1]);
			pos ++;
		} else if (!strcmp(argv[pos], "-fbb")){
			ns.fb_bound = atoi (argv[pos+1]);
			pos ++;
		} else if (!strcmp(argv[pos], "-lpb")){
			ns.lp_bound = atoi (argv[pos+1]);
			pos++;
		} else if (!strcmp(argv[pos], "-M")){
			ns.M = atoi (argv[pos+1]);
			pos++;
		} else if (!strcmp(argv[pos], "-np")){
			ns.lp_bound = 0;
		} else {
			mpz_set_str (n, argv[pos], 10);
			nspecd = 1;
		}
		pos ++;
	}
	if (!nspecd){
		mpz_inp_str (n, stdin, 10);
	}

	nsieve_init (&ns, n);

	factor (&ns);

	printf ("\nTiming summary: \
		 \n\tInitialization:   %ld \
		 \n\tSieving:          %ld \
		 \n\tMatrix solving:   %ld \
		 \n\tFactor deduction: %ld\n", ns.timing.init_time, ns.timing.sieve_time, ns.timing.matsolve_time, ns.timing.facdeduct_time);
}
