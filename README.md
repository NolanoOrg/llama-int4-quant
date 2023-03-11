# LLaMa-int4 inference (proof of concept, not for production use)

This is built on `gq` branch of [ggml](https://github.com/ggerganov/ggml) C++ library. Commit history was lost and is not available.

## Usage for LLaMa inference

0. Clone the repo `git clone https://github.com/NolanoOrg/llama-int4-mac`
1. Keep the LLaMa model weights in `../llama/save/7B`. Create a config.json file inside it with following keys `{"vocab_size": 32000, "n_positions": 2048, "n_embd": 4096, "n_hddn": 11008, "n_head": 32, "n_layer": 32, "rotary_dim": 64}`, modify as per your model.
2. Convert using to ggml format `cd examples/llama && python3 convert-h5-to-ggml.py ../../../llama/save/7B/ 1` -- 1 denotes fp16, 0 denotes fp32.
3. `mkdir build` if not already present.
4. `cd build && cmake .. && make llama-quantize && make llama`.
5. Quantize the model `./bin/llama-quantize ../../llama/save/7B/llama-f32.binf16.bin ../models/llama7B-0-quant4.bin 2`.
6. Switch to python app directory `cd ../app` and edit the prompt in `tok_prompt.py`.
7. Run the model `python3 tok_prompt.py && ./bin/llama --model_path ../models/llama7B-0-quant4.bin --vocab ../vocab/llama_vocab_clean.txt -n [NO_OF_TOKENS_TO_GENERATE]`.

## Changes to the original codebase

Most of the codebase for LLaMa is in examples/llama.

I edited the src/ggml.c and its header file to add new activation functions, RMSNorm and fix rope embedding among other things.

I also made changes to examples/utils.h and examples/utils.cpp to add the LLaMa model.

# Credits:

This codebase is based on the ggml library.

