import streamlit as st
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTCASES_DIR = ROOT / "testcases"

st.set_page_config(page_title="MPI Testcase Dashboard", layout="wide")

st.title("MPI Testcase Dashboard")

st.markdown(
    "Write an MPI C program below, save it into the `testcases/` folder and compile it with the project's runtime."
)

default_code = r'''#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    printf("Hello from rank %d/%d\n", rank, size);

    MPI_Finalize();
    return 0;
}
'''

# Initialize filename only once
if "filename" not in st.session_state:
    st.session_state.filename = f"user_test_{int(time.time())}"

# Initialize code only once
if "code" not in st.session_state:
    st.session_state.code = default_code

col1, col2 = st.columns([2, 1])

with col1:
    st.text_input(
        "Filename (without .c)",
        key="filename"
    )

    st.text_area(
        "C source code",
        key="code",
        height=400
    )

    submit = st.button("Save")

with col2:
    st.markdown("### Actions")
    st.write("Saved tests go to:")
    st.code(str(TESTCASES_DIR))

    st.markdown("---")

    st.markdown("### Build Options")
    use_pass = st.checkbox(
        "Use instrumentation pass (MPISAN_USE_PASS=1)",
        value=False
    )

    st.info(
        "The dashboard only saves the source file. "
        "Run your project's build/run scripts separately."
    )

if submit:
    filename = st.session_state.filename.strip()

    if not filename:
        st.error("Please provide a filename.")
    else:
        TESTCASES_DIR.mkdir(parents=True, exist_ok=True)

        out_src = TESTCASES_DIR / f"{filename}.c"

        with open(out_src, "w", encoding="utf-8") as f:
            f.write(st.session_state.code)

        st.success(f"✅ Saved as `{out_src.name}`")

st.markdown("---")
st.markdown("## Existing Testcases")

if TESTCASES_DIR.exists():
    files = sorted(TESTCASES_DIR.glob("*.c"))

    if files:
        for f in files:
            st.write(f"📄 {f.name}")
    else:
        st.info("No testcases found.")
else:
    st.info("No testcases directory found yet.")