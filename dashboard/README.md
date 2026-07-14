# MPI Testcase Dashboard

This small Streamlit app lets you create MPI C testcases and compile them against the project's runtime and (optionally) the MPISAN LLVM pass.

Quick start

1. Create a Python venv and install dependencies:

```bash
python -m venv .venv
source .venv/bin/activate   # or `.venv\Scripts\activate` on Windows
pip install -r dashboard/requirements.txt
```

2. Run the dashboard from the repository root:

```bash
streamlit run dashboard/app.py
```

Notes

- The dashboard invokes `mpicc`, `clang`, and `opt` (when using the instrumentation pass). Ensure these are available in your PATH.
- The runtime library and pass plugin are expected in `build/libmpisan_rt.a` and `build/MPISanitizerPass.so`. If missing, run `./build.sh` in a Bash environment to build them.
- Saved testcases are written to the `testcases/` directory and compiled there.
