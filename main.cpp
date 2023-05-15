#include <iostream>
#include <memory>
#include <cstdlib>
#include <chrono>

#include "tilt/ir/op.h"
#include "tilt/codegen/printer.h"
#include "tilt/codegen/loopgen.h"
#include "tilt/codegen/llvmgen.h"
#include "tilt/codegen/vinstr.h"
#include "tilt/engine/engine.h"
#include "tilt/builder/tilder.h"

using namespace tilt;
using namespace tilt::tilder;
using namespace std;
using namespace std::chrono;

template<typename T>
struct Event {
    int64_t st;
    int64_t et;
    T payload;
};

template<typename InTy, typename OutTy>
using QueryFn = function<vector<Event<OutTy>>(vector<Event<InTy>>)>;

Op _Select(_sym in, function<Expr(Expr)> sel_expr)
{
    auto e = in[_pt(0)];
    auto e_sym = _sym("e", e);
    auto sel = sel_expr(_get(e_sym, 0));
    auto sel_sym = _sym("sel", sel);
    auto sel_op = _op(
        _iter(0, 1),
        Params{ in },
        SymTable{ {e_sym, e}, {sel_sym, sel} },
        _exists(e_sym),
        sel_sym);
    return sel_op;
}

Op _NestedSelect(_sym in, int64_t w)
{
    auto window = in[_win(-w, 0)];
    auto window_sym = _sym("win", window);
    auto inner_sel = _Select(
        window_sym,
        [](Expr e) { return _add(e, _i32(10)); });
    auto inner_sel_sym = _sym("inner_sel", inner_sel);
    auto outer_sel = _op(
        _iter(0, w),
        Params{ in },
        SymTable{ {window_sym, window}, {inner_sel_sym, inner_sel} },
        _exists(window_sym),
        inner_sel_sym);
    return outer_sel;
}

void run_op(string query_name, Op op, ts_t st, ts_t et, region_t* out_reg, region_t* in_reg)
{
    auto op_sym = _sym(query_name, op);
    auto loop = LoopGen::Build(op_sym, op.get());

    cout << ">> Completed loop gen" << endl;

    auto jit = ExecEngine::Get();
    auto& llctx = jit->GetCtx();

    auto llmod = LLVMGen::Build(loop, llctx);

    cout << ">> Completed llvm gen" << endl;

    // FIXME: check out IR
    cout << ">> TILT IR\n";
    cout << IRPrinter::Build(op);
    cout << "\n\n";
    cout << ">> LOOP IR\n";
    cout << IRPrinter::Build(loop);
    cout << "\n\n";
    cout << ">> LLVM IR\n";
    cout << IRPrinter::Build(llmod.get());
    cout << "\n\n";

    jit->AddModule(move(llmod));

    auto loop_addr = (region_t* (*)(ts_t, ts_t, region_t*, region_t*)) jit->Lookup(loop->get_name());

    loop_addr(st, et, out_reg, in_reg);
}

template<typename InTy, typename OutTy>
void op_test(string query_name, Op op, ts_t st, ts_t et, QueryFn<InTy, OutTy> query_fn, vector<Event<InTy>> input)
{
    auto in_st = input[0].st;
    auto true_out = query_fn(input);

    region_t in_reg;
    auto in_tl = vector<ival_t>(input.size());
    auto in_data = vector<InTy>(input.size());
    auto in_data_ptr = reinterpret_cast<char*>(in_data.data());
    init_region(&in_reg, in_st, get_buf_size(input.size()), in_tl.data(), in_data_ptr);
    for (size_t i = 0; i < input.size(); i++) {
        auto t = input[i].et;
        commit_data(&in_reg, t);
        auto* ptr = reinterpret_cast<InTy*>(fetch(&in_reg, t, get_end_idx(&in_reg), sizeof(InTy)));
        *ptr = input[i].payload;
    }

    region_t out_reg;
    auto out_tl = vector<ival_t>(true_out.size());
    auto out_data = vector<OutTy>(true_out.size());
    auto out_data_ptr = reinterpret_cast<char*>(out_data.data());
    init_region(&out_reg, st, get_buf_size(true_out.size()), out_tl.data(), out_data_ptr);

    run_op(query_name, op, st, et, &out_reg, &in_reg);

    for (size_t i = 0; i < true_out.size(); i++) {
        // auto true_st = true_out[i].st;
        // auto true_et = true_out[i].et;
        auto true_payload = true_out[i].payload;
        // auto out_st = out_tl[i].t;
        // auto out_et = out_st + out_tl[i].d;
        auto out_payload = out_data[i];

        // assert_eq(true_st, out_st);
        // assert_eq(true_et, out_et);
        cout << true_payload << " " << out_payload << endl;
    }
}


// template<typename InTy, typename OutTy>
// void op_test(string query_name, Op op, ts_t st, ts_t et, QueryFn<InTy, OutTy> query_fn, vector<Event<InTy>> input)
// {
//     auto in_st = input[0].st;
//     auto true_out = query_fn(input);

//     cout << "INPUT SIZE: " << input.size() << endl;
//     cout << "OUTPUT SIZE: " << true_out.size() << endl;

//     region_t in_reg;
//     auto in_data = vector<InTy>(input.size());
//     auto in_data_ptr = reinterpret_cast<char*>(in_data.data());
//     init_region(&in_reg, in_st, get_buf_size(input.size()), nullptr, in_data_ptr);
//     for (size_t i = 0; i < input.size(); i++) {
//         auto* ptr = reinterpret_cast<InTy*>(in_reg.data + i * sizeof(InTy));
//         *ptr = input[i].payload;
//     }

//     region_t out_reg;
//     auto out_data = vector<OutTy>(true_out.size());
//     auto out_data_ptr = reinterpret_cast<char*>(out_data.data());
//     init_region(&out_reg, st, get_buf_size(true_out.size()), nullptr, out_data_ptr);

//     run_op(query_name, op, st, et, &out_reg, &in_reg);

//     for (size_t i = 0; i < true_out.size(); i++) {
//         auto true_payload = true_out[i].payload;
//         auto out_payload = out_data[i];

//         cout << "EXPECTED: " << true_payload << " | ACTUAL: " << out_payload << endl;
//     }

//     cout << "Completed op_test" << endl;
// }

// template<typename InTy, typename OutTy>
// void unary_op_test(string query_name, Op op, ts_t st, ts_t et, QueryFn<InTy, OutTy> query_fn, size_t len, int64_t dur)
// {
//     std::srand(time(nullptr));

//     vector<Event<InTy>> input(len);
//     for (size_t i = 0; i < len; i++) {
//         int64_t st = dur * i;
//         int64_t et = st + dur;
//         InTy payload = static_cast<InTy>(std::rand() / static_cast<double>(RAND_MAX / 100000));
//         input[i] = {st, et, payload};
//     }

//     cout << "Starting unary_op_test" << endl;
//     op_test<InTy, OutTy>(query_name, op, st, et, query_fn, input);
//     cout << "Completed unary_op_test" << endl;
// }

template<typename InTy, typename OutTy>
void unary_op_test(string query_name, Op op, ts_t st, ts_t et, QueryFn<InTy, OutTy> query_fn, size_t len, int64_t dur)
{
    std::srand(time(nullptr));

    vector<Event<InTy>> input(len);
    for (size_t i = 0; i < len; i++) {
        int64_t st = dur * i;
        int64_t et = st + dur;
        InTy payload = static_cast<InTy>(std::rand() / static_cast<double>(RAND_MAX / 100000));
        input[i] = {st, et, payload};
    }

    op_test<InTy, OutTy>(query_name, op, st, et, query_fn, input);
}


template<typename InTy, typename OutTy>
void nested_select_test(string query_name, function<Expr(Expr)> sel_expr, function<OutTy(InTy)> sel_fn)
{
    size_t len = 1000;
    int64_t dur = 1;

    auto in_sym = _sym("in", tilt::Type(types::STRUCT<InTy>(), _iter(0, -1)));
    auto sel_op = _NestedSelect(in_sym, 10);

    auto sel_query_fn = [sel_fn] (vector<Event<InTy>> in) {
        vector<Event<OutTy>> out;

        for (size_t i = 0; i < in.size(); i++) {
            out.push_back({in[i].st, in[i].et, sel_fn(in[i].payload)});
        }

        return move(out);
    };

    unary_op_test<InTy, OutTy>(query_name, sel_op, 0, len * dur, sel_query_fn, len, dur);
    cout << "Completed nested_select_test" << endl;
}

int main(int argc, char* argv[])
{
    nested_select_test<int32_t, int32_t>("nested_iadd",
        [] (Expr s) { return _add(s, _i32(10)); },
        [] (int32_t s) { return s + 10; });
    cout << "Completed add_test" << endl;
    return 0;
}
