#include <iostream>
#include <memory>
#include <cstdlib>
#include <chrono>

#include "tilt/pass/printer.h"
#include "tilt/pass/codegen/loopgen.h"
#include "tilt/pass/codegen/llvmgen.h"
#include "tilt/pass/codegen/vinstr.h"
#include "tilt/engine/engine.h"
#include "tilt/builder/tilder.h"

using namespace tilt;
using namespace tilt::tilder;
using namespace std;
using namespace std::chrono;

Op _Select(_sym in, function<Expr(_sym)> selector)
{
    auto e = in[_pt(0)];
    auto e_sym = _sym("e", e); 
    auto res = selector(e_sym);
    auto res_sym = _sym("res", res);
    auto sel_op = _op(
        _iter(0, 1), 
        Params{in},
        SymTable{{e_sym, e}, {res_sym, res}},
        _exists(e_sym),
        res_sym);
    return sel_op;
}

Op _NestedSelect(_sym in, int w)
{
    auto win = in[_win(-w, 0)];
    auto win_sym = _sym("win", win);
    auto inner_sel = _Select(win_sym, [](_sym e) { return e + _f32(10); });
    auto inner_sel_sym = _sym("inner_sel", inner_sel);
    auto sel_op = _op(
        _iter(0, w),
        Params{in},
        SymTable{{win_sym, win}, {inner_sel_sym, inner_sel}},
        _true(),
        inner_sel_sym);
    return sel_op;
}

int main(int argc, char* argv[])
{
    int dlen = (argc > 1) ? atoi(argv[1]) : 30;
    int len = (argc > 2) ? atoi(argv[2]) : 10;

    // input stream
    auto in_sym = _sym("in", tilt::Type(types::FLOAT32, _iter(0, -1)));

    auto query_op = _NestedSelect(in_sym, 10);
    //auto query_op = _Select(in_sym, [](_sym e) { return e + _f32(10); });
    auto query_op_sym = _sym("query", query_op);
    cout << endl << "TiLT IR:" << endl;
    cout << IRPrinter::Build(query_op) << endl;

    auto loop = LoopGen::Build(query_op_sym, query_op.get());
    cout << endl << "Loop IR:" << endl;
    cout << IRPrinter::Build(loop);

    auto jit = ExecEngine::Get();
    auto& llctx = jit->GetCtx();
    auto llmod = LLVMGen::Build(loop, llctx);
    cout << endl << "LLVM IR:" << endl;
    cout << IRPrinter::Build(llmod.get()) << endl;

    jit->AddModule(move(llmod));

    auto loop_addr = (region_t* (*)(ts_t, ts_t, region_t*, region_t*)) jit->Lookup(loop->get_name());

    uint32_t dur = 1;
    auto buf_size = get_buf_size(dlen);

    auto in_data = new float[dlen];
    region_t in_reg;
    init_region(&in_reg, 0, buf_size, reinterpret_cast<char*>(in_data));
    for (int i = 0; i < dlen; i++) {
        auto t = dur * i;
        commit_data(&in_reg, t);
        auto* ptr = reinterpret_cast<float*>(fetch(&in_reg, t, sizeof(float)));
        *ptr = i%1000;
    }

    auto out_data = new float[dlen];
    region_t out_reg;
    init_region(&out_reg, 0, buf_size, reinterpret_cast<char*>(out_data));

    auto start_time = high_resolution_clock::now();
    auto* res_reg = loop_addr(0, dur*dlen, &out_reg, &in_reg);
    auto end_time = high_resolution_clock::now();

    int out_count = dlen;
    if (argc == 1) {
        for (int i = 0; i < dlen; i++) {
            cout << in_data[i] << " -> " << out_data[i] << endl;
        }
    }

    delete [] in_data;
    delete [] out_data;

    auto time = duration_cast<microseconds>(end_time - start_time).count();
    cout << "Data size: " << out_count << " Time: " << time << endl;

    return 0;
}
