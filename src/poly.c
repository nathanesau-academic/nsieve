#include "poly.h"

void polygroup_init (poly_group_t *pg, nsieve_t *ns){
	mpz_init (pg->a);
	pg->ainverses = (uint32_t *) malloc(ns->fb_len * sizeof(uint32_t));
	pg->bvals = (mpz_t *) malloc( ns->bvals * sizeof (mpz_t *));
	for (int i=0; i < ns->bvals; i++){
		mpz_init (pg->bvals[i]);
	}
}

void polygroup_free (poly_group_t *pg, nsieve_t *ns){
	mpz_clear (pg->a);
	free (pg->ainverses);
	for (int i=0; i < ns->bvals; i++){
		mpz_clear (pg->bvals[i]);
	}
	free (pg->bvals);
}

void poly_init (poly_t *p){
	mpz_inits (p->a, p->b, p->c, p->istart, NULL);
}

void poly_free (poly_t *p){
	mpz_clears (p->a, p->b, p->c, p->istart, NULL);
}

uint32_t pi (uint32_t x){	// rough estimate of the prime-counting function. Implemented as x/ln(x).
	return (uint32_t) (x / log(x));
}

void mpz_prevprime (mpz_t res){
	mpz_sub_ui(res, res, 1);
	while (!mpz_probab_prime_p(res, 10)){	// this will be fast for the even values, since mpz_probab_prime_p does a little trial division first.
		mpz_sub_ui(res, res, 1);
	}
}

/* This routine will pick a value of k and an appropriate set of g values. 
 * We want to pick the largest value of k that will give us enough possible polynomials. If k is too large, the g values become very
 * small, and to produce enough distinct A, we end up with most A well outside the optimal range for our N and M. We take the following approach:
 *
 * Pick a minimum number of polynomials, minP, say 10^6 or 10^7. Pick a rather large k. Also pick a value 0 < c < 1. c will constrain
 * the range of allowable g values. Let Aopt be the optimal value of A (that is, sqrt(2N)/M). Then we only take g between (c * Aopt)^(1/k) and
 * (1/c * Aopt)^(1/k). 
 *
 * Determine the smallest q such that q C k > minP. (Note that these q can be precomputed (they don't depend on N) and stored as compile-time
 * constants). The range must contain at least q valid values of g. Instead of actually computing the number
 * of valid g, we can estimate it, using an approximation to pi(x), the prime-counting function. If we have a range, [gmin..gmax], then we can
 * estimate the # of valid g as
 * 	
 * 	   pi(gmax) - pi(gmin)
 * #g ~=   ------------------            pi(x) ~= x / ln(x), for instance.
 * 	            2
 * 
 * where the 2 is present because only about half of the time will N be a quadratic residue mod p. 
 *
 * If #g is too small (i.e., less than q), decrement k and try again.
 *
 * Once a suitable k has been found, find q/2 values of g on either side of Aopt^(1/k). This is the gpool.
*/

// using minP = 10^6. Thank you to Mathematica for producing these values. 
uint32_t q[] = {1000000, 1414, 182, 71, 44, 33, 28, 25, 24, 23, 23, 23};	// beyond k=12, q actually increases again! (this makes sense if you think about it enough)

void gpool_init (poly_gpool_t *gp, nsieve_t *ns){
	mpz_t aopt, temp, g;
	mpz_inits(aopt, temp, g, NULL);

	mpz_mul_ui    (aopt, ns->N, 2);
	mpz_sqrt      (aopt, aopt);
	mpz_tdiv_q_ui (aopt, aopt, ns->M);	// yay, aopt = sqrt(2N)/M.

	int c_num = 6;
	int c_den = 10;	// c = 0.6

	int k = 12;
	while (k >= 1) {
		// compute gmin and gmax
		mpz_mul_ui(temp, aopt, c_num);
		mpz_tdiv_q_ui(temp, temp, c_den);
		mpz_root (temp, temp, k);
		uint32_t gmin = mpz_get_ui(temp);

		mpz_mul_ui(temp, aopt, c_den);
		mpz_tdiv_q_ui(temp, temp, c_num);
		mpz_root (temp, temp, k);
		uint32_t gmax = mpz_get_ui(temp);

		// approximate #g and compare to q[k-1]	(since q[0] has data for k=1)
		uint32_t approx_ng = (pi(gmax) - pi(gmin)) / 2;
		if (approx_ng >= q[k-1]){	// then this value of k is good.
			break;
		}
		k--;
	}
	if (k == 0){
		printf("Fatal error: no values of k were deemed viable.\n");
		exit(1);
	}
	mpz_root (temp, aopt, k);	// temp contains the 'central' value for our range.
	int ng = q[k-1];	// we must find this many g values.
	gp->gpool = (uint32_t *) malloc (ng * sizeof (uint32_t));	// allocate the g pool

	int pos = ng/2;
	mpz_set (g, temp);
	while (pos < ng){
		mpz_nextprime(g, g);
		if (mpz_kronecker (ns->N, g) == 1){
			gp->gpool[pos] = mpz_get_ui(g);
			pos++;
		}
	}
	// now do the lower half
	pos = ng/2 - 1;
	mpz_set (g, temp);
	while (pos >= 0){
		mpz_prevprime(g);	// note that this is my function, not GMP's
		if (mpz_kronecker (ns->N, g) == 1){
			gp->gpool[pos] = mpz_get_ui(g);
			pos--;
		}
	}
	// initialize the rest of the fields
	gp->center = mpz_get_ui (temp);	// I don't know if I need this field or not. Probably not.
	gp->ng = ng;
	gp->frogs = (uint32_t *)(malloc(k * sizeof(uint32_t)));	// these hold indices into gpool.
	for (int i=0; i < k; i++){
		gp->frogs[i] = i;
	}
	ns->k = k;
	ns->bvals = 1 << (k-1);
}
	

void generate_polygroup (poly_group_t *pg, nsieve_t *ns){
	/* This is a tricky one. First we must choose A, by picking k primes g_i, and multiplying them together.
	 * Then we need to find all of the values of B which satisfy  B^2 = N (mod a). There will be 2^(k-1) of them. 
	 * Then we will compute the values of A^-1 (mod p) for each p in the factor base. This is really a precomputation
	 * to speed up the computation of Q(x) = 0 (mod p). 
	*/
	
	// We want A to be about sqrt (2N) / M. Thus the primes we're looking at should be around [sqrt(2n)/M)] ^ (1/k).
	// However, there is no particular requirement that they be of comparable sizes, so this is just a rough guide.
	

	
	/* Now that we've chosen A, we can compute the values of B. The goal is to produce all values of B which satisfy 
	 * B^2 = N (mod A). Since A is composite, this is a little tricky, but we know it's prime factorization (that's how we
	 * constructed it). First, we find for each g_i the values r_i_1 and r_i_2 such that r_i^2 = N (mod g_i). Then, we set
	 * up a system of modular congruences 
	 *
	 * 	b ~= r_0_? (mod g_0)
	 * 	b ~= r_1_? (mod g_1)
	 * 	...
	 * 	b ~= r_k_? (mod g_k).
	 *
	 * which can be solved via the Chinese Remainder Theorem. The Wolfram Mathworld page on the CRT provides a helpful formula:
	 *
	 * 	b ~= r_0_? * j_0 * (A / g_0) + ... r_k_? * j_k * (A / g_k)  (mod A). We just take B = that expression.
	 *
	 * where the j_i are determined as 
	 * 	
	 * 	j_i * (A / g_i) ~= 1 (mod g_i), or in other words, j_i is the inverse of (A / g_i) (mod g_i).
	 *
	 *
	 * We repeat this procedure, 2^k times, taking every possible combination of which r_i_? we select 
	 * (? is independently replaced by either 1 or 2 in each congruence). 
	 * We disregard values of B > A/2, since these are the negations of other, smaller B. Hence, we end up with 2^(k-1) values for B.
	*/
	int k = ns->k;

	uint32_t r[k][2];

	// first find r_i^2 ~= N (mod g_i)
	for (int i=0; i<k; i++){
		r[i][0] = find_root (ns->N, pg->gvals[i]);
		r[i][1] = pg->gvals[i] - r[i][0];
	}

	// now do the main loop
	mpz_t t1, t2;		// temps
	mpz_inits (t1, t2, NULL);
	for (int z = 0; z < (1 << k); z++){	// the ith bit of z will control which root r_i_? is used for the current B computation.
		mpz_set_ui (pg->bvals[z], 0);	// clear the B-value
		for (int i=0; i < k; i++){	// for each G-value
			mpz_set_ui (t2, pg->gvals[i]);			// t2 = g_i
			mpz_divexact_ui (t1, pg->a, pg->gvals[i]);	// t1 = A / g_i
			mpz_invert (t2, t1, t2);	// t2 = inverse of (A / g_i) (mod g_i) = j_i
			mpz_mul (t2, t2, t1);		// t2 = j_i * (A / g_i)
			// pick the root according to the i'th least siginificant bit of z.
			int root = (z & (1 << i)) == 0 ? 0 : 1;
			mpz_mul_ui (t2, t2, r[i][root]);
			mpz_add (pg->bvals[z], pg->bvals[z], t2);
		}
		mpz_mod (pg->bvals[z], pg->bvals[z], pg->a);	// take the sum mod A.
	}

	// Now that we've chosen A and determined the values of B, we compute A^-1 (mod p) for each prime in the factor base.
	mpz_t p, temp;
	mpz_inits (p, temp, NULL);
	for (int i=0; i<ns->fb_len; i++){
		mpz_set_ui (p, ns->fb[i]);
		int invertable = mpz_invert (temp, pg->a, p);
		if (invertable){	// good
			pg -> ainverses[i] = mpz_get_ui (temp);
		} else {
			// hmmm. This is interesting. This should happen only when fb[i] == g_j. I don't know yet what exactly should happen here.
		}
	}
	mpz_clears (p, temp, NULL);
}

void generate_poly (poly_t *p, poly_group_t *pg, nsieve_t *ns, int i){
	mpz_set(p->a, pg->a);
	mpz_set (p->b, pg->bvals[i]);
	// compute C = (b^2 - n) / a
	mpz_mul (p->c, p->b, p->b);	 // C = b^2
	mpz_sub (p->c, p->c, ns->N);	 // C = b^2 - N
	mpz_divexact (p->c, p->c, p->a); // C = (b^2 - N) / a
}

void poly (mpz_t res, poly_t *p, uint32_t offset){
	// evaluate Ax^2 + 2Bx + C  where x = istart + offset
	// = (((A * X) + 2B) * X) + C
	mpz_t x;
	mpz_inits  (x, NULL);
	mpz_add_ui (x, p->istart, offset);	// x = istart + offset
	mpz_mul    (res, p->a, x);
	mpz_add    (res, res, p->b);
	mpz_add    (res, res, p->b);
	mpz_mul    (res, res, x);
	mpz_add    (res, res, p->c);
	mpz_clear  (x);
}
