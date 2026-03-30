# MiniCoin

Privacy coin made in C. One day hopefully with a purpose beyond "educational" :)

Some notes for me right now:

cmake:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
```

Build with clang/clang++ (I don't want to deal with the gcc/MSVC shenangans right now)

Use:
```bash
./bin/minicoin_node <-mine>
```

Main Hashing Algorithm: SHA256d (double SHA256) for now.
Proof-of-Work Algorithm: Autolykos2