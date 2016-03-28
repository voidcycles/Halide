#include <algorithm>
#include <cmath>

#include "FuseGPUThreadLoops.h"
#include "CodeGen_GPU_Dev.h"
#include "IR.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Bounds.h"
#include "Substitute.h"
#include "IREquality.h"
#include "Simplify.h"
#include "IRPrinter.h"

namespace Halide {
namespace Internal {

using std::map;
using std::vector;
using std::string;
using std::sort;

namespace {
string thread_names[] = {"__thread_id_x", "__thread_id_y", "__thread_id_z", "__thread_id_w"};
string block_names[] = {"__block_id_x", "__block_id_y", "__block_id_z", "__block_id_w"};
string shared_mem_name = "__shared";
}

class InjectThreadBarriers : public IRMutator {
    bool in_threads;

    using IRMutator::visit;

    Stmt barrier;

    void visit(const For *op) {
        bool old_in_threads = in_threads;
        if (CodeGen_GPU_Dev::is_gpu_thread_var(op->name)) {
            in_threads = true;
        }

        IRMutator::visit(op);

        in_threads = old_in_threads;
    }

    void visit(const ProducerConsumer *op) {
        if (!in_threads) {
            Stmt produce = mutate(op->produce);
            if (!is_no_op(produce)) {
                produce = Block::make(produce, barrier);
            }

            Stmt update;
            if (op->update.defined()) {
                update = mutate(op->update);
                if (!is_no_op(update)) {
                    update = Block::make(update, barrier);
                }
            }

            Stmt consume = mutate(op->consume);

            stmt = ProducerConsumer::make(op->name, produce, update, consume);
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Block *op) {
        if (!in_threads && op->rest.defined()) {
            Stmt first = mutate(op->first);
            Stmt rest = mutate(op->rest);
            stmt = Block::make(Block::make(first, barrier), rest);
        } else {
            IRMutator::visit(op);
        }
    }

public:
    InjectThreadBarriers() : in_threads(false) {
        barrier =
            Evaluate::make(Call::make(Int(32), "halide_gpu_thread_barrier",
                                      vector<Expr>(), Call::Extern));
    }
};


class ExtractBlockSize : public IRVisitor {
    Expr block_extent[4];

    Scope<Interval> scope;

    using IRVisitor::visit;

    void found_for(int dim, Expr extent) {
        internal_assert(dim >= 0 && dim < 4);
        if (!block_extent[dim].defined()) {
            block_extent[dim] = extent;
        } else {
            block_extent[dim] = simplify(Max::make(extent, block_extent[dim]));
        }
    }

    void visit(const For *op) {
        Interval ie = bounds_of_expr_in_scope(op->extent, scope);
        Interval im = bounds_of_expr_in_scope(op->min, scope);

        for (int i = 0; i < 4; i++) {
            if (ends_with(op->name, thread_names[i])) {
                found_for(i, ie.max);
            }
        }

        scope.push(op->name, Interval(im.min, im.max + ie.max - 1));
        IRVisitor::visit(op);
        scope.pop(op->name);
    }

    void visit(const LetStmt *op) {
        Interval i = bounds_of_expr_in_scope(op->value, scope);
        scope.push(op->name, i);
        op->body.accept(this);
        scope.pop(op->name);
    }

public:
    int dimensions() const {
        for (int i = 0; i < 4; i++) {
            if (!block_extent[i].defined()) {
                return i;
            }
        }
        return 4;
    }

    Expr extent(int d) const {
        return block_extent[d];
    }

    void max_over_blocks(const Scope<Interval> &scope) {
        for (int i = 0; i < 4; i++) {
            if (block_extent[i].defined()) {
                block_extent[i] = simplify(block_extent[i]);
                block_extent[i] = bounds_of_expr_in_scope(block_extent[i], scope).max;
            }
        }
    }

};

class NormalizeDimensionality : public IRMutator {
    using IRMutator::visit;

    const ExtractBlockSize &block_size;
    const DeviceAPI device_api;

    int depth;
    int max_depth;

    Stmt wrap(Stmt s) {
        if (depth != 0) {
            return mutate(s);
        }
        max_depth = 0;
        s = mutate(s);
        if (is_no_op(s)) {
            return s;
        }
        while (max_depth < block_size.dimensions()) {
            string name = thread_names[max_depth];
            s = For::make("." + name, 0, 1, ForType::Parallel, device_api, s);
            max_depth++;
        }
        return s;
    }

    void visit(const ProducerConsumer *op) {
        Stmt produce = wrap(op->produce);
        Stmt update;
        if (op->update.defined()) {
            update = wrap(op->update);
        }
        Stmt consume = wrap(op->consume);

        if (produce.same_as(op->produce) &&
            update.same_as(op->update) &&
            consume.same_as(op->consume)) {
            stmt = op;
        } else {
            stmt = ProducerConsumer::make(op->name, produce, update, consume);
        }
    }

    void visit(const Block *op) {
        Stmt first = wrap(op->first);

        Stmt rest;
        if (op->rest.defined()) {
            rest = wrap(op->rest);
        }

        if (first.same_as(op->first) &&
            rest.same_as(op->rest)) {
            stmt = op;
        } else {
            stmt = Block::make(first, rest);
        }
    }

    void visit(const For *op) {
        if (CodeGen_GPU_Dev::is_gpu_thread_var(op->name)) {
            depth++;
            if (depth > max_depth) {
                max_depth = depth;
            }
            IRMutator::visit(op);
            depth--;
        } else {
            IRMutator::visit(op);
        }
    }

public:
    NormalizeDimensionality(const ExtractBlockSize &e, DeviceAPI device_api)
      : block_size(e), device_api(device_api), depth(0), max_depth(0) {}
};

class ReplaceForWithIf : public IRMutator {
    using IRMutator::visit;

    const ExtractBlockSize &block_size;

    void visit(const For *op) {
        if (CodeGen_GPU_Dev::is_gpu_thread_var(op->name)) {
            int dim;
            for (dim = 0; dim < 4; dim++) {
                if (ends_with(op->name, thread_names[dim])) {
                    break;
                }
            }

            internal_assert(dim >= 0 && dim < block_size.dimensions());

            Expr var = Variable::make(Int(32), "." + thread_names[dim]);
            internal_assert(is_zero(op->min));
            Stmt body = mutate(op->body);

            body = substitute(op->name, var, body);

            if (equal(op->extent, block_size.extent(dim))) {
                stmt = body;
            } else {
                Expr cond = var < op->extent;
                stmt = IfThenElse::make(cond, body, Stmt());
            }
        } else {
            IRMutator::visit(op);
        }
    }

public:
    ReplaceForWithIf(const ExtractBlockSize &e) : block_size(e) {}
};

class ExtractSharedAllocations : public IRMutator {
    using IRMutator::visit;

    struct IntInterval {
        IntInterval() : IntInterval(0, 0) {}
        IntInterval(int min, int max) : min(min), max(max) {}
        int min;
        int max;
    };

    struct SharedAllocation {
        string name;
        Type type;
        Expr size;
        IntInterval liveness;
    };

    struct AllocPairs {
        AllocPairs() {}
        AllocPairs(const SharedAllocation &alloc) : max_type_bytes(alloc.type.bytes()) {
            max_size_bytes = simplify(alloc.type.bytes() * alloc.size);
            pairs.push_back(alloc);
        }

        void insert(const SharedAllocation &alloc) {
            max_type_bytes = std::max(max_type_bytes, alloc.type.bytes());
            max_size_bytes = simplify(max(max_size_bytes, simplify(alloc.size * alloc.type.bytes())));
            pairs.push_back(alloc);
        }

        bool is_free(int stage) const {
            return pairs.back().liveness.max < stage;
        }

        int max_type_bytes;
        Expr max_size_bytes; // In bytes
        vector<SharedAllocation> pairs;
    };

    vector<SharedAllocation> allocations;

    Scope<Interval> scope;

    map<string, IntInterval> shared;

    bool in_threads;

    int barrier_stage;

    int max_byte_size;

    const DeviceAPI device_api;

    void visit(const For *op) {
        if (CodeGen_GPU_Dev::is_gpu_thread_var(op->name)) {
            bool old = in_threads;
            in_threads = true;
            IRMutator::visit(op);
            in_threads = old;
        } else {
            Interval min_bounds = bounds_of_expr_in_scope(op->min, scope);
            Interval extent_bounds = bounds_of_expr_in_scope(op->extent, scope);
            Interval bounds(min_bounds.min, min_bounds.max + extent_bounds.max - 1);
            scope.push(op->name, bounds);
            IRMutator::visit(op);
            scope.pop(op->name);
        }
    }


    void visit(const ProducerConsumer *op) {
        if (!in_threads) {
            Stmt produce = mutate(op->produce);
            if (!is_no_op(produce)) {
                barrier_stage++;
            }
            Stmt update;
            if (op->update.defined()) {
                update = mutate(op->update);
                if (!is_no_op(update)) {
                    barrier_stage++;
                }
            }
            Stmt consume = mutate(op->consume);

            if (produce.same_as(op->produce) &&
                update.same_as(op->update) &&
                consume.same_as(op->consume)) {
                stmt = op;
            } else {
                stmt = ProducerConsumer::make(op->name, produce, update, consume);
            }
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Block *op) {
        if (!in_threads && op->rest.defined()) {
            Stmt first = mutate(op->first);
            Stmt rest = mutate(op->rest);

            if (first.same_as(op->first) &&
                rest.same_as(op->rest)) {
                stmt = op;
            } else {
                stmt = Block::make(first, rest);
            }
        } else {
            IRMutator::visit(op);
        }
    }



    void visit(const Allocate *op) {
        user_assert(!op->new_expr.defined()) << "Allocate node inside GPU kernel has custom new expression.\n" <<
            "(Memoization is not supported inside GPU kernels at present.)\n";

        if (in_threads) {
            IRMutator::visit(op);
            return;
        }

        shared.emplace(op->name, IntInterval(barrier_stage, barrier_stage));
        IRMutator::visit(op);
        op = stmt.as<Allocate>();
        internal_assert(op);

        max_byte_size = std::max(max_byte_size, op->type.bytes());

        SharedAllocation alloc;
        alloc.name = op->name;
        alloc.type = op->type;
        alloc.liveness = shared[op->name];
        alloc.size = 1;
        for (size_t i = 0; i < op->extents.size(); i++) {
            alloc.size *= op->extents[i];
        }
        alloc.size = bounds_of_expr_in_scope(simplify(alloc.size), scope).max;
        allocations.push_back(alloc);
        stmt = op->body;

        shared.erase(op->name);
    }

    void visit(const Load *op) {
        Expr index = mutate(op->index);
        if (shared.count(op->name)) {
            shared[op->name].max = barrier_stage;
            if (device_api == DeviceAPI::OpenGLCompute) {
                expr = Load::make(op->type, shared_mem_name + "_" + op->name, index, op->image, op->param);
            } else {
                Expr base = Variable::make(Int(32), op->name + ".shared_offset");
                expr = Load::make(op->type, shared_mem_name, base + index, op->image, op->param);
            }

        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Store *op) {
        if (shared.count(op->name)) {
            shared[op->name].max = barrier_stage;
            Expr index = mutate(op->index);
            Expr value = mutate(op->value);
            if (device_api == DeviceAPI::OpenGLCompute) {
                stmt = Store::make(shared_mem_name + "_" + op->name, value, index);
            } else {
                Expr base = Variable::make(Int(32), op->name + ".shared_offset");
                stmt = Store::make(shared_mem_name, value, base + index);
            }
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const LetStmt *op) {
        if (in_threads) {
            IRMutator::visit(op);
            return;
        }

        Expr value = mutate(op->value);
        Interval bounds = bounds_of_expr_in_scope(value, scope);
        bounds.min = simplify(bounds.min);
        bounds.max = simplify(bounds.max);
        scope.push(op->name, bounds);
        Stmt body = mutate(op->body);
        scope.pop(op->name);

        if (op->body.same_as(body) && op->value.same_as(value)) {
            stmt = op;
        } else {
            stmt = LetStmt::make(op->name, value, body);
        }
    }

    // Return index to mem_allocs where 'alloc' should go. Return -1
    // if there isn't any.
    int find_best_fit(const vector<AllocPairs>& mem_allocs,
                      const vector<int>& free_spaces,
                      const SharedAllocation& alloc, int stage) {
        int index = -1;

       if (!is_const(alloc.size)) {
            // Start search from the most-recently freed space. Prioritize non-const
            // free space over the constant one. If we can't find any non-const free
            // space, return index to the most-recently freed constant space.
            for (int i = free_spaces.size() - 1; i >= 0; --i) {
                // Only need to check the back of the vector since we always insert
                // the most recent allocation at the back.
                internal_assert(mem_allocs[free_spaces[i]].is_free(stage));

                if (!is_const(mem_allocs[free_spaces[i]].max_size_bytes)) {
                    return i;
                } else if (index == -1) {
                    index = i;
                }
            }
        } else {
            // Start search from the most-recently freed space. Prioritize constant
            // free space over the non-constant one. If we can't find any const free
            // space, return index to the most-recently freed non-constant space;
            // otherwise, return index to the smallest const free space that fits the
            // allocation size.
            int64_t diff = -1;
            for (int i = free_spaces.size() - 1; i >= 0; --i) {
                // Only need to check the back of the vector since we always insert
                // the most recent allocation at the back.
                internal_assert(mem_allocs[free_spaces[i]].is_free(stage));

                if (index == -1) {
                    index = i;
                } else if (is_const(mem_allocs[free_spaces[i]].max_size_bytes)) {
                    Expr size = alloc.size * alloc.type.bytes();
                    Expr dist = mem_allocs[free_spaces[i]].max_size_bytes - size;
                    const int64_t *current_diff = as_const_int(dist * dist);
                    internal_assert(current_diff != NULL);
                    if (*current_diff < diff) {
                        diff = *current_diff;
                        index = i;
                    }
                }
            }
        }

        return index;
    }

    // Sort based on the ascending order of the min liveness stage; if equal,
    // sort based on the ascending order of the max liveness stage.
    vector<AllocPairs> allocate_funcs(vector<SharedAllocation> &allocations) {
        sort(allocations.begin(), allocations.end(),
            [](const SharedAllocation &lhs, const SharedAllocation &rhs){
                if (lhs.liveness.min < rhs.liveness.min) {
                    return true;
                } else if (lhs.liveness.min == rhs.liveness.min) {
                    return lhs.liveness.max < rhs.liveness.max;
                }
                return false;
            }
        );

        //debug(0) << "AFTER SORTING: \n";
        //print_alloc();

        vector<AllocPairs> mem_allocs;
        vector<int> free_spaces; // Contains index to free spaces in mem_allocs
        int start_idx = 0;

        for (int stage = 0; stage < barrier_stage; ++stage) {
            for (int i = start_idx; i < (int)allocations.size(); ++i) {
                if (allocations[i].liveness.min > stage) {
                    break;
                } else if (allocations[i].liveness.min == stage) { // Allocate
                    int index = find_best_fit(mem_allocs, free_spaces, allocations[i], stage);
                    if (index != -1) {
                        mem_allocs[index].insert(allocations[i]);
                        free_spaces.erase(free_spaces.begin() + index);
                    } else {
                        mem_allocs.push_back(AllocPairs(allocations[i]));
                    }
                } else if (allocations[i].liveness.max == stage - 1) { // Free
                    free_spaces.push_back(i);
                    start_idx = i+1;
                }
            }
        }

        return mem_allocs;
    }

public:
    Stmt rewrap(Stmt s) {

        if (device_api == DeviceAPI::OpenGLCompute) {

            // Individual shared allocations.
            for (SharedAllocation alloc : allocations) {
                s = Allocate::make(shared_mem_name + "_" + alloc.name,
                                   alloc.type, {alloc.size}, const_true(), s);
            }
        } else {
            // One big combined shared allocation.

            vector<AllocPairs> mem_allocs = allocate_funcs(allocations);

            // Sort the allocations by the max size in bytes of the primitive
            // types in the pair. Because the type sizes are then decreasing powers of
            // two, doing this guarantees that all allocations are aligned
            // to then element type as long as the original one is aligned
            // to the widest type.
            sort(mem_allocs.begin(), mem_allocs.end(),
                [](const AllocPairs &lhs, const AllocPairs &rhs){
                    return lhs.max_type_bytes > rhs.max_type_bytes;
                }
            );

            SharedAllocation sentinel;
            sentinel.name = "sentinel";
            sentinel.type = UInt(8);
            sentinel.size = 0;
            mem_allocs.push_back(AllocPairs(sentinel));

            // Add a dummy allocation at the end to get the total size
            Expr total_size = Variable::make(Int(32), "pair_" + std::to_string(mem_allocs.size()-1) + ".shared_offset");
            s = Allocate::make(shared_mem_name, UInt(8), {total_size}, const_true(), s);

            // Define an offset for each allocation. The offsets are in
            // elements, not bytes, so that the stores and loads can use
            // them directly.
            for (int i = (int)(mem_allocs.size()) - 1; i >= 0; i--) {
                Expr pair_offset = Variable::make(Int(32), "pair_" + std::to_string(i) + ".shared_offset");

                for (SharedAllocation &alloc : mem_allocs[i].pairs) {
                    int new_elem_size = alloc.type.bytes();
                    Expr offset = (pair_offset / new_elem_size);
                    s = LetStmt::make(alloc.name + ".shared_offset", simplify(offset), s);
                }

                Expr offset = 0;
                if (i > 0) {
                    offset = Variable::make(Int(32), "pair_" + std::to_string(i-1) + ".shared_offset");
                    int new_elem_size = mem_allocs[i].max_type_bytes;
                    offset += (((mem_allocs[i-1].max_size_bytes + new_elem_size - 1)/new_elem_size)*new_elem_size);
                }
                s = LetStmt::make("pair_" + std::to_string(i) + ".shared_offset", simplify(offset), s);
            }
        }

        return s;
    }

    void print_alloc() {
        for (const auto &iter : allocations) {
            debug(0) << "\tAlloc: " << iter.name << "; type: " << iter.type << "; size: " << simplify(iter.size)
                     << "; liveness: [" << iter.liveness.min << ", " << iter.liveness.max << "]\n";
        }
    }

    ExtractSharedAllocations(DeviceAPI d) : in_threads(false), barrier_stage(0)
                                            , max_byte_size(0), device_api(d) {}
};

class FuseGPUThreadLoops : public IRMutator {
    using IRMutator::visit;

    Scope<Interval> scope;

    bool in_gpu_loop = false;

    void visit(const LetStmt *op) {
        if (in_gpu_loop) {
            scope.push(op->name, bounds_of_expr_in_scope(op->value, scope));
            IRMutator::visit(op);
            scope.pop(op->name);
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const For *op) {
         if (op->device_api == DeviceAPI::GLSL) {
            stmt = op;
            return;
        }

        user_assert(!(CodeGen_GPU_Dev::is_gpu_thread_var(op->name)))
            << "Loops over GPU thread variable: \"" << op->name
            << "\" is outside of any loop over a GPU block variable. "
            << "This schedule is malformed. There must be a GPU block "
            << "variable, and it must reordered to be outside all GPU "
            << "thread variables.\n";

        bool should_pop = false;
        bool old_in_gpu_loop = in_gpu_loop;
        if (CodeGen_GPU_Dev::is_gpu_block_var(op->name)) {
            Interval im = bounds_of_expr_in_scope(op->min, scope);
            Interval ie = bounds_of_expr_in_scope(op->extent, scope);
            scope.push(op->name, Interval(im.min, im.max + ie.max - 1));
            should_pop = true;
            in_gpu_loop = true;
        }

        if (ends_with(op->name, ".__block_id_x")) {

            Stmt body = op->body;

            ExtractBlockSize e;
            body.accept(&e);
            e.max_over_blocks(scope);

            debug(3) << "Fusing thread block:\n" << body << "\n\n";

            NormalizeDimensionality n(e, op->device_api);
            body = n.mutate(body);

            debug(3) << "Normalized dimensionality:\n" << body << "\n\n";

            ExtractSharedAllocations h(op->device_api);
            body = h.mutate(body);
            h.print_alloc();

            debug(3) << "Pulled out shared allocations:\n" << body << "\n\n";

            InjectThreadBarriers i;
            body = i.mutate(body);

            debug(3) << "Injected synchronization:\n" << body << "\n\n";

            ReplaceForWithIf f(e);
            body = f.mutate(body);

            debug(3) << "Replaced for with if:\n" << body << "\n\n";

            // Rewrap the whole thing in the loop over threads
            for (int i = 0; i < e.dimensions(); i++) {
                body = For::make("." + thread_names[i], 0, e.extent(i), ForType::Parallel, op->device_api, body);
            }

            // There at least needs to be a loop over __thread_id_x as a marker for codegen
            if (e.dimensions() == 0) {
                body = For::make(".__thread_id_x", 0, 1, ForType::Parallel, op->device_api, body);
            }

            debug(3) << "Rewrapped in for loops:\n" << body << "\n\n";

            // Add back in the shared allocations
            body = h.rewrap(body);

            debug(3) << "Add back in shared allocations:\n" << body << "\n\n";

            if (body.same_as(op->body)) {
                stmt = op;
            } else {
                stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
            }
        } else {
            IRMutator::visit(op);
        }

        if (should_pop) {
            scope.pop(op->name);
        }
        in_gpu_loop = old_in_gpu_loop;
    }
};

class ZeroGPULoopMins : public IRMutator {
    bool in_non_glsl_gpu;
    using IRMutator::visit;

    void visit(const For *op) {
        bool old_in_non_glsl_gpu = in_non_glsl_gpu;

        in_non_glsl_gpu = (in_non_glsl_gpu && op->device_api == DeviceAPI::Parent) ||
          (op->device_api == DeviceAPI::CUDA) || (op->device_api == DeviceAPI::OpenCL) ||
          (op->device_api == DeviceAPI::Metal);

        IRMutator::visit(op);
        if (CodeGen_GPU_Dev::is_gpu_var(op->name) && !is_zero(op->min)) {
            op = stmt.as<For>();
            internal_assert(op);
            Expr adjusted = Variable::make(Int(32), op->name) + op->min;
            Stmt body = substitute(op->name, adjusted, op->body);
            stmt = For::make(op->name, 0, op->extent, op->for_type, op->device_api, body);
        }

        in_non_glsl_gpu = old_in_non_glsl_gpu;
    }

public:
    ZeroGPULoopMins() : in_non_glsl_gpu(false) { }
};

class ValidateGPULoopNesting : public IRVisitor {
    int gpu_block_depth = 0, gpu_thread_depth = 0;
    string innermost_block_var, innermost_thread_var;

    using IRVisitor::visit;

    void visit(const For *op) {
        string old_innermost_block_var  = innermost_block_var;
        string old_innermost_thread_var = innermost_thread_var;
        int old_gpu_block_depth  = gpu_block_depth;
        int old_gpu_thread_depth = gpu_thread_depth;

        for (int i = 1; i <= 4; i++) {
            if (ends_with(op->name, block_names[4-i])) {
                user_assert(i > gpu_block_depth)
                    << "Invalid schedule: Loop over " << op->name
                    << " cannot be inside of loop over " << innermost_block_var << "\n";
                user_assert(gpu_thread_depth == 0)
                    << "Invalid schedule: Loop over " << op->name
                    << " cannot be inside of loop over " << innermost_thread_var << "\n";
                innermost_block_var = op->name;
                gpu_block_depth = i;
            }
            if (ends_with(op->name, thread_names[4-i])) {
                user_assert(i > gpu_thread_depth)
                    << "Invalid schedule: Loop over " << op->name
                    << " cannot be inside of loop over " << innermost_thread_var << "\n";
                user_assert(gpu_block_depth > 0)
                    << "Invalid schedule: Loop over " << op->name
                    << " must be inside a loop over gpu blocks\n";
                innermost_thread_var = op->name;
                gpu_thread_depth = i;
            }
        }
        IRVisitor::visit(op);

        innermost_block_var  = old_innermost_block_var;
        innermost_thread_var = old_innermost_thread_var;
        gpu_block_depth  = old_gpu_block_depth;
        gpu_thread_depth = old_gpu_thread_depth;
    }
};

// Also used by InjectImageIntrinsics
Stmt zero_gpu_loop_mins(Stmt s) {
    return ZeroGPULoopMins().mutate(s);
}

Stmt fuse_gpu_thread_loops(Stmt s) {
    ValidateGPULoopNesting validate;
    s.accept(&validate);
    s = ZeroGPULoopMins().mutate(s);
    s = FuseGPUThreadLoops().mutate(s);
    return s;
}

}
}
