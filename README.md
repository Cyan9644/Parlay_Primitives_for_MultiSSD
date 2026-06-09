This github is a platform for the CAAR REU studies on implementing Parlay primtitives for the MultiSSD setting.

Some ideas for primitives that could port well (written by Alex):

parlay::find_if: The basic parlay version uses a doubling sort to find the first index such that the value satisfies a given unary predicate. This is obviously very easy if the data is stored in blocks, but we have to assume that it's distributed across SSDs, which will incur random read costs. On the bright side, there is no writing involved so write amplification from random access is not an issue. CPU work is probably light.

parlay::delayed_map: This should be simple because MultiSSD already has a map function, but if the data is distributed across SSDs, call-time evaluation could incur multiple SSD read costs. Maybe at least a small cache?

parlay::delayed_map: This should be easy to implement but probably won't be great for the MultiSSD setting -- we already have map, which actually instantiates the transformed array. The issue here is that accessing each indivdual element could incur SSD read costs.


parlay::tabulate:: Should be fine.


parlay::delayed_tabulate: Should be fine.


parlay::iota: Should be easy -- this is effectively a map:

auto x = parlay::map(input, [&](long i){
return i;

}

parlay::scan && parlay:scan_inclusive: These are not so easy.

parlay::pack && parlay::pack_index: This is essentially just a filter that acts on a boolean list instead of a predicate. If we can do filter, we can probably do pack.

parlay::flatten: Should be simple as long as ops occur in DRAM, maybe lots of random reads though

parlay::set_union: This is not a primitive, but there are two interesting ways to think about this problem as an application.
If the data is unsorted, you can join the two sets in a sequence, flatten, and then remove duplicates
if the data is sorted, find where the median of the first sequence (at spot i) would fit in the second sequence (at spot j)
and recur on the subproblems 
set_union(set1.cut(0, i), set2.cut(0,j);
set_union (set1.cut(i, n1), set2.cut(j, n2));
adjusting indices accordingly.


parlay::group_by_key



parlay::histogram

parlay::lexicographical_compare

parlay::lower/upper bound: Good luck with this one



parlay::to_sequence
