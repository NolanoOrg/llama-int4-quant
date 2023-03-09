# ggml

## Usage for LLaMa
1. Let the LLaMa model weights be in `../llama/save/7B`. Create a config.json file inside it with following keys `{"vocab_size": 32000, "n_positions": 2048, "n_embd": 4096, "n_hddn": 11008, "n_head": 32, "n_layer": 32, "rotary_dim": 64}`, modify as per your model.
2. Convert using to ggml format `cd examples/llama && python3 convert-h5-to-ggml.py ../../../llama/save/7B/ 1` -- 1 denotes fp16, 0 denotes fp32
3. `mkdir build` if not already present
4. `cd build && cmake .. && make llama-quantize && make llama`
5. Quantize the model `./bin/llama-quantize ../../llama/save/7B/llama--f32.binf16.bin ../models/llama7B-0-quant4.bin 2`
6. Run the model `./bin/llama -m ../models/llama7B-0-quant4.bin -v ../models/llama_vocab.txt -p "This is an example" -t 8 -n [NO_OF_TOKENS]`

##
Tensor library for machine learning

***Note that this project is under development and not ready for production use. \
Some of the development is currently happening in the [whisper.cpp](https://github.com/ggerganov/whisper.cpp) repo***

## Features

- Written in C
- 16-bit float support
- Automatic differentiation (WIP in progress)
- ADAM and L-BFGS optimizers
- Optimized for Apple silicon via NEON intrinsics and Accelerate framework
- On x86 architectures utilzes AVX intrinsics
- No third-party dependencies
- Zero memory allocations during runtime

## Roadmap

- [X] Example of GPT-2 inference [examples/gpt-2](https://github.com/ggerganov/ggml/tree/master/examples/gpt-2)
- [X] Example of GPT-J inference [examples/gpt-j](https://github.com/ggerganov/ggml/tree/master/examples/gpt-j)
- [X] Example of Whisper inference [examples/whisper](https://github.com/ggerganov/ggml/tree/master/examples/whisper)
- [ ] Support 4-bit integer quantization https://github.com/ggerganov/ggml/pull/27
- [ ] Example of FLAN-T5 inference https://github.com/ggerganov/ggml/pull/12
- [ ] Example of LLaMA inference
- [ ] Example of RWKV inference

## Whisper inference (example)

With ggml you can efficiently run [Whisper](examples/whisper) inference on the CPU.

Memory requirements:

| Model  | Disk   | Mem     |
| ---    | ---    | ---     |
| tiny   |  75 MB | ~280 MB |
| base   | 142 MB | ~430 MB |
| small  | 466 MB | ~1.0 GB |
| medium | 1.5 GB | ~2.6 GB |
| large  | 2.9 GB | ~4.7 GB |

## GPT inference (example)

With ggml you can efficiently run [GPT-2](examples/gpt-2) and [GPT-J](examples/gpt-j) inference on the CPU.

Here is how to run the example programs:

```bash
# Build ggml + examples
git clone https://github.com/ggerganov/ggml
cd ggml
mkdir build && cd build
cmake ..
make -j4 gpt-2 gpt-j

# Run the GPT-2 small 117M model
../examples/gpt-2/download-ggml-model.sh 117M
./bin/gpt-2 -m models/gpt-2-117M/ggml-model.bin -p "This is an example"

# Run the GPT-J 6B model (requires 12GB disk space and 16GB CPU RAM)
../examples/gpt-j/download-ggml-model.sh 6B
./bin/gpt-j -m models/gpt-j-6B/ggml-model.bin -p "This is an example"
```

The inference speeds that I get for the different models on my 32GB MacBook M1 Pro are as follows:

| Model | Size  | Time / Token |
| ---   | ---   | ---    |
| GPT-2 |  117M |   5 ms |
| GPT-2 |  345M |  12 ms |
| GPT-2 |  774M |  23 ms |
| GPT-2 | 1558M |  42 ms |
| ---   | ---   | ---    |
| GPT-J |    6B | 125 ms |

For more information, checkout the corresponding programs in the [examples](examples) folder.
