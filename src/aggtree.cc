#include <click/config.h>
#include "aggtree.hh"
#include "aggwtree.hh"
#include <click/confparse.hh>
#include <click/error.hh>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef HAVE_BYTEORDER_H
#include <byteorder.h>
#else
static inline uint32_t bswap_32(uint32_t u) {
    return ((u >> 24) | ((u & 0xff0000) >> 8) | ((u & 0xff00) << 8) | ((u & 0xff) << 24));
}
#endif


void
AggregateTree::initialize_root()
{
    if (!(_root = new_node())) {
	fprintf(stderr, "out of memory!\n");
	abort();
    }
    _root->aggregate = 0;
    _root->count = 0;
    _root->child[0] = _root->child[1] = 0;
    _num_nonzero = 0;
}

void
AggregateTree::copy_nodes(const Node *n, uint32_t mask)
{
    if (n->count)
	add(n->aggregate & mask, n->count);
    if (n->child[0]) {
	copy_nodes(n->child[0], mask);
	copy_nodes(n->child[1], mask);
    }
}

AggregateTree::AggregateTree()
    : _free(0)
{
    initialize_root();
}

AggregateTree::AggregateTree(const AggregateTree &o)
    : _free(0)
{
    initialize_root();
    copy_nodes(o._root);
}

AggregateTree::AggregateTree(const AggregateWTree &o)
    : _free(0)
{
    initialize_root();
    copy_nodes(o._root);
}

AggregateTree::~AggregateTree()
{
    kill_all_nodes();
}

AggregateTree &
AggregateTree::operator=(const AggregateTree &o)
{
    if (&o != this) {
	kill_all_nodes();
	initialize_root();
	copy_nodes(o._root);
    }
    return *this;
}

AggregateTree &
AggregateTree::operator=(const AggregateWTree &o)
{
    kill_all_nodes();
    initialize_root();
    copy_nodes(o._root);
    return *this;
}

AggregateTree &
AggregateTree::operator+=(const AggregateTree &o)
{
    assert(&o != this);
    copy_nodes(o._root);
    return *this;
}

AggregateTree::Node *
AggregateTree::new_node_block()
{
    assert(!_free);
    int block_size = 1024;
    Node *block = new Node[block_size];
    if (!block)
	return 0;
    _blocks.push_back(block);
    for (int i = 1; i < block_size - 1; i++)
	block[i].child[0] = &block[i+1];
    block[block_size - 1].child[0] = 0;
    _free = &block[1];
    return &block[0];
}

void
AggregateTree::kill_all_nodes()
{
    for (int i = 0; i < _blocks.size(); i++)
	delete _blocks[i];
    _blocks.clear();
    _root = _free = 0;
}

int
mask_to_prefix(uint32_t mask)
{
    if (mask == 0xFFFFFFFFU)
	return 32;
    int possible_p = bi_ffs(~mask) - 1;
    uint32_t new_mask = prefix_to_mask(possible_p);
    return (new_mask == mask ? possible_p : -1);
}

// from tcpdpriv
int
bi_ffs(uint32_t value)
{
    int add = 0;
    static uint8_t bvals[] = { 0, 4, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1 };

    if ((value & 0xFFFF0000) == 0) {
	if (value == 0) {	/* zero input ==> zero output */
	    return 0;
	}
	add += 16;
    } else {
	value >>= 16;
    }
    if ((value & 0xFF00) == 0) {
	add += 8;
    } else {
	value >>= 8;
    }
    if ((value & 0xF0) == 0) {
	add += 4;
    } else {
	value >>= 4;
    }
    return add + bvals[value & 0xf];
}

//
// check to see tree is OK
//

static uint32_t
node_ok(AggregateTree::Node *n, int last_swivel, ErrorHandler *errh)
{
    if (n->child[0] && n->child[1]) {
	int swivel = bi_ffs(n->child[0]->aggregate ^ n->child[1]->aggregate);
	if (swivel <= last_swivel)
	    return errh->error("%x: bad swivel %d <= %d (%x-%x)", n->aggregate, swivel, last_swivel, n->child[0]->aggregate, n->child[1]->aggregate);
	
	uint32_t mask = (swivel == 1 ? 0 : 0xFFFFFFFFU << (33 - swivel));
	if ((n->child[0]->aggregate & mask) != (n->aggregate & mask))
	    return errh->error("%x: left child doesn't match upper bits (swivel %d)", n->aggregate, swivel);
	if ((n->child[1]->aggregate & mask) != (n->aggregate & mask))
	    return errh->error("%x: right child doesn't match upper bits (swivel %d)", n->aggregate, swivel);

	mask = (1 << (32 - swivel));
	if ((n->child[0]->aggregate & mask) != 0)
	    return errh->error("%x: left child swivel bit one (swivel %d)", n->aggregate, swivel);
	if ((n->child[1]->aggregate & mask) == 0)
	    return errh->error("%x: right child swivel bit zero (swivel %d)", n->aggregate, swivel);

	mask = (swivel == 1 ? 0xFFFFFFFFU : (1 << (32 - swivel)) - 1);
	if (n->aggregate & mask)
	    return errh->error("%x: lower bits nonzero (swivel %d)", n->aggregate, swivel);

	int ok1 = node_ok(n->child[0], swivel, errh);
	int ok2 = node_ok(n->child[1], swivel, errh);
	int local_nnz = (n->count ? 1 : 0);
	return ok1 + ok2 + local_nnz;
	
    } else if (n->child[0] || n->child[1])
	return errh->error("%x: only one live child", n->aggregate);
    else
	return (n->count ? 1 : 0);
}

bool
AggregateTree::ok(ErrorHandler *errh) const
{
    if (!errh)
	errh = ErrorHandler::default_handler();

    int before = errh->nerrors();
    uint32_t nnz = node_ok(_root, -1, errh);
    if (errh->nerrors() == before && nnz != _num_nonzero)
	errh->error("bad num_nonzero: nominally %u, calculated %u", _num_nonzero, nnz);
    
    return (errh->nerrors() == before ? 0 : -1);
}


//
// TREE CONSTRUCTION
//

AggregateTree::Node *
AggregateTree::make_peer(uint32_t a, Node *n)
{
    /*
     * become a peer
     * algo: create two nodes, the two peers.  leave orig node as
     * the parent of the two new ones.
     */

    Node *down[2];
    if (!(down[0] = new_node()))
	return 0;
    if (!(down[1] = new_node())) {
	free_node(down[0]);
	return 0;
    }

    // swivel is first bit 'a' and 'old->input' differ
    int swivel = bi_ffs(a ^ n->aggregate);
    // bitvalue is the value of that bit of 'a'
    int bitvalue = (a >> (32 - swivel)) & 1;
    // mask masks off all bits before swivel
    uint32_t mask = (swivel == 1 ? 0 : (0xFFFFFFFFU << (33 - swivel)));

    down[bitvalue]->aggregate = a;
    down[bitvalue]->count = 0;
    down[bitvalue]->child[0] = down[bitvalue]->child[1] = 0;

    *down[1 - bitvalue] = *n;	/* copy orig node down one level */

    n->aggregate = (down[0]->aggregate & mask);
    if (down[0]->aggregate == n->aggregate) {
	n->count = down[0]->count;
	down[0]->count = 0;
    } else
	n->count = 0;
    n->child[0] = down[0];	/* point to children */
    n->child[1] = down[1];

    return (n->aggregate == a ? n : down[bitvalue]);
}

AggregateTree::Node *
AggregateTree::find_node(uint32_t a)
{
    // straight outta tcpdpriv
    Node *n = _root;
    while (n) {
	if (n->aggregate == a)
	    return n;
	if (!n->child[0])
	    n = make_peer(a, n);
	else {
	    // swivel is the first bit in which the two children differ
	    int swivel = bi_ffs(n->child[0]->aggregate ^ n->child[1]->aggregate);
	    if (bi_ffs(a ^ n->aggregate) < swivel) // input differs earlier
		n = make_peer(a, n);
	    else if (a & (1 << (32 - swivel)))
		n = n->child[1];
	    else
		n = n->child[0];
	}
    }
    
    fprintf(stderr, "AggregateTree: out of memory!\n");
    return 0;
}

AggregateTree::Node *
AggregateTree::find_existing_node(uint32_t a) const
{
    // straight outta tcpdpriv
    Node *n = _root;
    while (n) {
	if (n->aggregate == a)
	    return n;
	if (!n->child[0])
	    return 0;
	else {
	    // swivel is the first bit in which the two children differ
	    int swivel = bi_ffs(n->child[0]->aggregate ^ n->child[1]->aggregate);
	    if (bi_ffs(a ^ n->aggregate) < swivel) // input differs earlier
		return 0;
	    else if (a & (1 << (32 - swivel)))
		n = n->child[1];
	    else
		n = n->child[0];
	}
    }
    return 0;
}

void
AggregateTree::collapse_subtree(Node *root)
{
    if (root->child[0]) {
	collapse_subtree(root->child[0]);
	collapse_subtree(root->child[1]);
	
	int old_nnz = (root->count != 0) + (root->child[0]->count != 0) + (root->child[1]->count != 0);
	root->count += root->child[0]->count + root->child[1]->count;
	_num_nonzero += (root->count != 0) - old_nnz;
	
	free_node(root->child[0]);
	free_node(root->child[1]);
	root->child[0] = root->child[1] = 0;
    }
}

void
AggregateTree::node_zero_aggregate(Node *n, uint32_t mask, uint32_t value)
{
    if ((n->aggregate & mask) == value && n->count) {
	n->count = 0;
	_num_nonzero--;
    }
    if (n->child[0]) {
	// swivel is the first bit in which the two children differ
	int swivel = bi_ffs(n->child[0]->aggregate ^ n->child[1]->aggregate);
	uint32_t swivel_mask = (swivel == 1 ? 0 : 0xFFFFFFFFU << (33 - swivel)) & mask;
	if ((n->child[0]->aggregate & swivel_mask) == (value & swivel_mask))
	    node_zero_aggregate(n->child[0], mask, value);
	if ((n->child[1]->aggregate & swivel_mask) == (value & swivel_mask))
	    node_zero_aggregate(n->child[1], mask, value);
    }
}

void
AggregateTree::zero_aggregate(int prefix_len, uint32_t value)
{
    uint32_t mask = prefix_to_mask(prefix_len);
    assert((value & mask) == value);
    node_zero_aggregate(_root, mask, value);
    // assert(nnz_match(mask, value) == 0); // expensive
}

void
AggregateTree::zero_masked_aggregate(uint32_t mask, uint32_t value)
{
    int p = mask_to_prefix(mask);
    assert(p >= 0);
    zero_aggregate(p, value);
}

//
// COUNTING
//

static uint32_t
node_count_match(AggregateTree::Node *n, uint32_t mask, uint32_t value,
		 bool count)
{
    uint32_t result;
    if (n->count > 0 && (n->aggregate & mask) == value)
	result = (count ? n->count : 1);
    else
	result = 0;
    if (n->child[0])
	return (result
		+ node_count_match(n->child[0], mask, value, count)
		+ node_count_match(n->child[1], mask, value, count));
    else
	return result;
}

uint32_t
AggregateTree::nnz_match(uint32_t mask, uint32_t value) const
{
    assert((value & mask) == value);
    return node_count_match(_root, mask, value, false);
}


static void
node_sum_and_sum_sq(AggregateTree::Node *n, double *sum, double *sum_sq)
{
    *sum += n->count;
    *sum_sq += ((double)n->count) * n->count;
    if (n->child[0]) {
	node_sum_and_sum_sq(n->child[0], sum, sum_sq);
	node_sum_and_sum_sq(n->child[1], sum, sum_sq);
    }
}

void
AggregateTree::sum_and_sum_sq(double *sum, double *sum_sq) const
{
    double s = 0, ss = 0;
    node_sum_and_sum_sq(_root, &s, &ss);
    if (sum)
	*sum = s;
    if (sum_sq)
	*sum_sq = ss;
}


static uint32_t *
node_nonzero_sizes(AggregateTree::Node *n, uint32_t *vec)
{
    if (n->count)
	*vec++ = n->count;
    if (n->child[0]) {
	vec = node_nonzero_sizes(n->child[0], vec);
	vec = node_nonzero_sizes(n->child[1], vec);
    }
    return vec;
}

void
AggregateTree::nonzero_sizes(Vector<uint32_t> &vec) const
{
    vec.resize(_num_nonzero);
    if (_num_nonzero) {
	uint32_t *end_vec = node_nonzero_sizes(_root, &vec[0]);
	assert((uint32_t)(end_vec - &vec[0]) == _num_nonzero);
	(void) end_vec;
    }
}


//
// POSTERIZATION
//

static void
node_posterize(AggregateTree::Node *n)
{
    if (n->count)
	n->count = 1;
    if (n->child[0]) {
	node_posterize(n->child[0]);
	node_posterize(n->child[1]);
    }
}

void
AggregateTree::posterize()
{
    node_posterize(_root);
}


//
// SAMPLING
//

void
AggregateTree::node_sample(Node *n, uint32_t taking)
{
    if (n->count) {
	for (uint32_t i = n->count; i > 0; i--)
	    if (((uint32_t)random()) >= taking)
		n->count--;
	if (!n->count)
	    _num_nonzero--;
    }
    if (n->child[0]) {
	node_sample(n->child[0], taking);
	node_sample(n->child[1], taking);
    }
}

void
AggregateTree::sample(double sample_prob)
{
    assert(sample_prob >= 0);
    if (sample_prob < 1) {
	uint32_t taking = (uint32_t)(((uint32_t)RAND_MAX + 1) * sample_prob);
	node_sample(_root, taking);
    }
}


void
AggregateTree::node_cut_smaller(Node *n, uint32_t smallest)
{
    if (n->count && n->count < smallest) {
	n->count = 0;
	_num_nonzero--;
    }
    if (n->child[0]) {
	node_cut_smaller(n->child[0], smallest);
	node_cut_smaller(n->child[1], smallest);
    }
}

void
AggregateTree::cut_smaller(uint32_t smallest)
{
    node_cut_smaller(_root, smallest);
}


void
AggregateTree::node_cut_larger(Node *n, uint32_t largest)
{
    if (n->count && n->count >= largest) {
	n->count = 0;
	_num_nonzero--;
    }
    if (n->child[0]) {
	node_cut_larger(n->child[0], largest);
	node_cut_larger(n->child[1], largest);
    }
}

void
AggregateTree::cut_larger(uint32_t largest)
{
    node_cut_larger(_root, largest);
}


void
AggregateTree::node_cut_aggregates(Node *n, uint32_t mask, uint32_t &value, uint32_t &count, uint32_t size_boundary, bool smallerp, bool hostsp)
{
    if ((n->aggregate & mask) != value) {
	assert((n->aggregate & mask) > value);
	bool this_smallerp = (count < size_boundary);
	if (count && (smallerp == this_smallerp))
	    zero_masked_aggregate(mask, value);
	count = 0;
	value = (n->aggregate & mask);
    }
    count += (hostsp ? n->count != 0 : n->count);
    if (n->child[0]) {
	node_cut_aggregates(n->child[0], mask, value, count, size_boundary, smallerp, hostsp);
	node_cut_aggregates(n->child[1], mask, value, count, size_boundary, smallerp, hostsp);
    }
}

void
AggregateTree::cut_smaller_aggregates(int p, uint32_t smallest)
{
    uint32_t value = 0, count = 0;
    uint32_t mask = prefix_to_mask(p);
    node_cut_aggregates(_root, mask, value, count, smallest, true, false);
    if (count && count < smallest)
	zero_masked_aggregate(mask, value);
}

void
AggregateTree::cut_larger_aggregates(int p, uint32_t largest)
{
    uint32_t value = 0, count = 0;
    uint32_t mask = prefix_to_mask(p);
    node_cut_aggregates(_root, mask, value, count, largest, false, false);
    if (count && count >= largest)
	zero_masked_aggregate(mask, value);
}

void
AggregateTree::cut_smaller_host_aggregates(int p, uint32_t smallest)
{
    uint32_t value = 0, count = 0;
    uint32_t mask = prefix_to_mask(p);
    node_cut_aggregates(_root, mask, value, count, smallest, true, true);
    if (count && count < smallest)
	zero_masked_aggregate(mask, value);
}

void
AggregateTree::cut_larger_host_aggregates(int p, uint32_t largest)
{
    uint32_t value = 0, count = 0;
    uint32_t mask = prefix_to_mask(p);
    node_cut_aggregates(_root, mask, value, count, largest, false, true);
    if (count && count >= largest)
	zero_masked_aggregate(mask, value);
}


//
// PREFIXES
//

void
AggregateTree::node_prefixize(Node *n, int prefix)
{
    uint32_t mask = prefix_to_mask(prefix);

    if ((n->aggregate & mask) != n->aggregate) {
	collapse_subtree(n);
	n->aggregate &= mask;
    
    } else if (n->child[0]) {
	int swivel = bi_ffs(n->child[0]->aggregate ^ n->child[1]->aggregate);
	//ErrorHandler::default_handler()->message("%d", swivel);
	
	if (swivel <= prefix) {
	    node_prefixize(n->child[0], prefix);
	    node_prefixize(n->child[1], prefix);
	} else {
	    // assert((n->child[0]->aggregate & mask) == (n->child[1]->aggregate & mask)); -- true
	    collapse_subtree(n->child[0]);
	    collapse_subtree(n->child[1]);
	    if (n->child[0]->count && n->child[1]->count)
		_num_nonzero--;
	    n->child[0]->aggregate &= mask;
	    n->child[0]->count += n->child[1]->count;
	    n->child[1]->aggregate = n->child[0]->aggregate | 1;
	    n->child[1]->count = 0;
	}

	if (n->child[0]->aggregate == n->aggregate) {
	    if (n->child[0]->count && n->count)
		_num_nonzero--;
	    n->count += n->child[0]->count;
	    n->child[0]->count = 0;
	}
    }
}

void
AggregateTree::prefixize(int prefix_len)
{
    assert(prefix_len >= 0 && prefix_len <= 32);
    if (prefix_len < 32)
	node_prefixize(_root, prefix_len);
}

void
AggregateTree::make_prefix(int prefix_len, AggregateTree &t) const
{
    assert(prefix_len >= 0 && prefix_len <= 32);
    t.copy_nodes(_root, prefix_to_mask(prefix_len));
}

void
AggregateTree::nnz_in_prefixes(Vector<uint32_t> &out) const
{
    AggregateTree copy(*this);
    out.assign(33, 0);
    out[32] = nnz();
    for (int i = 31; i >= 0; i--) {
	copy.prefixize(i);
	out[i] = copy.nnz();
    }
}

void
AggregateTree::nnz_in_left_prefixes(Vector<uint32_t> &out) const
{
    AggregateTree copy(*this);
    out.assign(33, 0);
    out[32] = nnz_match(1, 0);
    for (int i = 31; i >= 0; i--) {
	copy.prefixize(i);
	out[i] = copy.nnz_match(1 << (32 - i), 0);
    }
}


//
// DISCRIMINATING PREFIXES
//

uint32_t
AggregateTree::node_to_discriminated_by(Node *n, const AggregateTree &prefix,
					uint32_t mask, bool count)
{
    uint32_t result = 0;
    
    if (n->count) {
	Node *nn = prefix.find_existing_node(n->aggregate & mask);
	assert(nn && nn->count >= n->count);
	if (nn->count > n->count) {
	    result += (count ? n->count : 1);
	    n->count = 0;
	}
    }

    if (n->child[0]) {
	result += node_to_discriminated_by(n->child[0], prefix, mask, count);
	result += node_to_discriminated_by(n->child[1], prefix, mask, count);
	if (!n->child[0]->child[0] && !n->child[1]->child[0]
	    && !n->child[0]->count && !n->child[1]->count)
	    collapse_subtree(n);
    }

    return result;
}

void
AggregateTree::nnz_discriminated_by_prefix(Vector<uint32_t> &out) const
{
    AggregateTree copy(*this);
    AggregateTree prefix(*this);
    out.assign(33, 0);

    for (int i = 32; i >= 1; i--) {
	prefix.prefixize(i - 1);
	out[i] = copy.node_to_discriminated_by(copy._root, prefix, prefix_to_mask(i - 1), false);
    }
}


//
// LEFT/RIGHT BALANCE
//

static void
node_balance(AggregateTree::Node *n, AggregateTree::Node **last,
	     uint32_t prefix_mask, FILE *f)
{
    if (n->count) {
	assert((n->aggregate & (~prefix_mask >> 1)) == 0);
	if (*last && ((*last)->aggregate & prefix_mask) == (n->aggregate & prefix_mask)) {
	    assert(n->aggregate & ~prefix_mask);
	    fprintf(f, "%u %u %u\n", (n->aggregate & prefix_mask), (*last)->count, n->count);
	    *last = 0;
	} else {
	    if (*last)
		fprintf(f, "%u %u 0\n", ((*last)->aggregate & prefix_mask), (*last)->count);
	    if (n->aggregate & ~prefix_mask) {
		fprintf(f, "%u 0 %u\n", (n->aggregate & prefix_mask), n->count);
		*last = 0;
	    } else
		*last = n;
	}
    }

    if (n->child[0]) {
	node_balance(n->child[0], last, prefix_mask, f);
	node_balance(n->child[1], last, prefix_mask, f);
    }
}

void
AggregateTree::balance(int p, FILE *f) const
{
    assert(p >= 0 && p <= 31);
    Node *last = 0;
    uint32_t prefix_mask = prefix_to_mask(p);
    node_balance(_root, &last, prefix_mask, f);
    if (last)
	fprintf(f, "%u %u 0\n", (last->aggregate & prefix_mask), last->count);
}


static void
node_balance_histogram(AggregateTree::Node *n, AggregateTree::Node **last,
		       uint32_t prefix_mask, double factor, Vector<uint32_t> &v)
{
    if (n->count) {
	assert((n->aggregate & (~prefix_mask >> 1)) == 0);
	if (*last && ((*last)->aggregate & prefix_mask) == (n->aggregate & prefix_mask)) {
	    assert(n->aggregate & ~prefix_mask);
	    double c0 = (double)((*last)->count);
	    double sum = c0 + (double)n->count;
	    if (c0 == 0)
		v[0]++;
	    else if (c0 == sum)
		v.back()++;
	    else {
		uint32_t which = (uint32_t)ceil((c0 / sum) * factor);
		v[which]++;
	    }
	    *last = 0;
	} else {
	    if (*last)
		v[0]++;
	    if (n->aggregate & ~prefix_mask) {
		v.back()++;
		*last = 0;
	    } else
		*last = n;
	}
    }

    if (n->child[0]) {
	node_balance_histogram(n->child[0], last, prefix_mask, factor, v);
	node_balance_histogram(n->child[1], last, prefix_mask, factor, v);
    }
}

void
AggregateTree::balance_histogram(int p, uint32_t nbuckets, Vector<uint32_t> &out) const
{
    assert(p >= 0 && p <= 31);
    out.assign(nbuckets + 1, 0);
    Node *last = 0;
    uint32_t prefix_mask = prefix_to_mask(p);
    node_balance_histogram(_root, &last, prefix_mask, nbuckets, out);
    if (last)
	out[0]++;
}


//
// WAVELET STUFF
//

static double
node_haar_energy(AggregateTree::Node *n, AggregateTree::Node **last,
		 uint32_t prefix_mask)
{
    double amt = 0;

    if (n->count) {
	if (!*last)
	    *last = n;
	else if ((n->aggregate & prefix_mask) != ((*last)->aggregate & prefix_mask)) {
	    amt = (*last)->count;
	    amt *= amt;
	    *last = n;
	} else {
	    amt = (double)(n->count) - (double)((*last)->count);
	    amt *= amt;
	    *last = 0;
	}
    }
    

    if (n->child[0]) {
	amt += node_haar_energy(n->child[0], last, prefix_mask);
	amt += node_haar_energy(n->child[1], last, prefix_mask);	
    }

    return amt;
}

void
AggregateTree::haar_wavelet_energy_coeff(Vector<double> &out) const
{
    AggregateTree copy(*this);
    out.assign(32, 0);
    
    for (int p = 31; p >= 0; p--) {
	Node *last = 0;
	double sum_sq_diff = node_haar_energy(copy._root, &last, prefix_to_mask(p));
	if (last)
	    sum_sq_diff += ((double)last->count) * last->count;
	
	out[p] = sum_sq_diff / 4294967296.0;
	
	copy.prefixize(p);
    }
}


//
// COMBINING TREES
//

static const AggregateTree::Node *
preorder_step(const AggregateTree::Node *other_stack[], int &other_pos)
{
    if (other_pos == 0)
	return 0;

    // if current node has a left child, return that
    const AggregateTree::Node *n = other_stack[other_pos - 1]->child[0];
    if (n) {
	other_stack[other_pos++] = n;
	return n;
    }

    // otherwise, back up and take a right child
    other_pos--;
    while (other_pos > 0 && other_stack[other_pos - 1]->child[1] == other_stack[other_pos])
	other_pos--;

    if (other_pos > 0) {
	const AggregateTree::Node *n = other_stack[other_pos - 1]->child[1];
	other_stack[other_pos++] = n;
	return n;
    } else
	return 0;
}

void
AggregateTree::node_keep_common_hosts(Node *n, const Node *other_stack[], int &other_pos, bool add)
{
    if (n->count) {
	const Node *other = (other_pos ? other_stack[other_pos - 1] : 0);
	while (other && other->aggregate < n->aggregate)
	    other = preorder_step(other_stack, other_pos);
	if (!other || other->aggregate > n->aggregate
	    || (other->aggregate == n->aggregate && other->count == 0)) {
	    n->count = 0;
	    _num_nonzero--;
	} else if (add && other->aggregate == n->aggregate)
	    n->count += other->count;
    }
    if (n->child[0]) {
	node_keep_common_hosts(n->child[0], other_stack, other_pos, add);
	node_keep_common_hosts(n->child[1], other_stack, other_pos, add);
    }
}

void
AggregateTree::keep_common_hosts(const AggregateTree &other, bool add)
{
    const Node *other_stack[32];
    other_stack[0] = other._root;
    int other_pos = 1;
    node_keep_common_hosts(_root, other_stack, other_pos, add);
}


//
// READING AND WRITING
//

static void
read_packed_file(FILE *f, AggregateTree *tree, int file_byte_order)
{
    uint32_t ubuf[BUFSIZ];
    if (file_byte_order == CLICK_BYTE_ORDER) {
	while (!feof(f) && !ferror(f)) {
	    size_t howmany = fread(ubuf, 8, BUFSIZ / 2, f);
	    for (size_t i = 0; i < howmany; i++)
		tree->add(ubuf[2*i], ubuf[2*i + 1]);
	}
    } else {
	while (!feof(f) && !ferror(f)) {
	    size_t howmany = fread(ubuf, 8, BUFSIZ / 2, f);
	    for (size_t i = 0; i < howmany; i++)
		tree->add(bswap_32(ubuf[2*i]), bswap_32(ubuf[2*i + 1]));
	}
    }
}

int
AggregateTree::read_file(FILE *f, ErrorHandler *errh)
{
    char s[BUFSIZ];
    uint32_t agg, value;
    while (fgets(s, BUFSIZ, f)) {
	if (strlen(s) == BUFSIZ - 1 && s[BUFSIZ - 2] != '\n')
	    return errh->error("line too long");
	if (s[0] == '$') {
	    if (strcmp(s, "$packed\n") == 0)
		read_packed_file(f, this, CLICK_BYTE_ORDER);
	    else if (strcmp(s, "$packed_le\n") == 0)
		read_packed_file(f, this, CLICK_LITTLE_ENDIAN);
	    else if (strcmp(s, "$packed_be\n") == 0)
		read_packed_file(f, this, CLICK_BIG_ENDIAN);
	} else if (sscanf(s, "%u %u", &agg, &value) == 2)
	    add(agg, value);
    }
    if (ferror(f))
	return errh->error("file error");
    return 0;
}

void
AggregateTree::write_batch(FILE *f, bool binary, uint32_t *buffer, int pos,
			   ErrorHandler *)
{
    if (binary)
	fwrite(buffer, sizeof(uint32_t), pos, f);
    else
	for (int i = 0; i < pos; i += 2)
	    fprintf(f, "%u %u\n", buffer[i], buffer[i+1]);
}

void
AggregateTree::write_nodes(Node *n, FILE *f, bool binary,
			   uint32_t *buffer, int &pos, int len,
			   ErrorHandler *errh)
{
    if (n->count > 0) {
	buffer[pos++] = n->aggregate;
	buffer[pos++] = n->count;
	if (pos == len) {
	    write_batch(f, binary, buffer, pos, errh);
	    pos = 0;
	}
    }

    if (n->child[0])
	write_nodes(n->child[0], f, binary, buffer, pos, len, errh);
    if (n->child[1])
	write_nodes(n->child[1], f, binary, buffer, pos, len, errh);
}

int
AggregateTree::write_file(FILE *f, bool binary, ErrorHandler *errh) const
{
    fprintf(f, "$num_nonzero %u\n", _num_nonzero);
#if CLICK_BYTE_ORDER == CLICK_BIG_ENDIAN
    if (binary)
	fprintf(f, "$packed_be\n");
#elif CLICK_BYTE_ORDER == CLICK_LITTLE_ENDIAN
    if (binary)
	fprintf(f, "$packed_le\n");
#else
    binary = false;
#endif
    
    uint32_t buf[1024];
    int pos = 0;
    write_nodes(_root, f, binary, buf, pos, 1024, errh);
    if (pos)
	write_batch(f, binary, buf, pos, errh);

    if (ferror(f))
	return errh->error("file error");
    else
	return 0;
}

// Vector instance
#include <click/vector.cc>
template class Vector<double>;
