#include "ggml/ggml.h"

#include "utils.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>

// typedef void (*message_callback)(const char*);
// extern "C" {
//     int add(int a, int b, message_callback callback) {
//         int result = a + b;
//         char message[100];
//         std::cout << "asdas";
//         snprintf(message, 100, "The result is %d", result);
//         callback(message);
//         return result;
//     }
// }



// default hparams (LLaMa 6.7B)
struct llama_hparams {
    int32_t n_vocab = 32000;
    int32_t n_ctx   = 2048;
    int32_t n_embd  = 4096;
    int32_t n_hddn  = 11008;
    int32_t n_head  = 32;
    int32_t n_layer = 32;
    int32_t n_rot   = 64;
    int32_t f16     = 1;
};

struct llama_layer {
    // normalization for input to attention
    struct ggml_tensor * attention_norm;

    // attention
    struct ggml_tensor * c_attn_q_proj_w;
    struct ggml_tensor * c_attn_k_proj_w;
    struct ggml_tensor * c_attn_v_proj_w;

    struct ggml_tensor * c_attn_proj_w;

    // normalization for input to ff
    struct ggml_tensor * c_ffn_norm;

    // ff
    struct ggml_tensor * c_feed_forward_w1;
    struct ggml_tensor * c_feed_forward_w2_trans; // transposed for efficiency
    struct ggml_tensor * c_feed_forward_w3;
};

struct llama_model {
    llama_hparams hparams;

    struct ggml_tensor * wte; // embedding

    std::vector<llama_layer> layers;

    struct ggml_tensor * final_norm; // normalization for input to lm head

    struct ggml_tensor * lmh_g; // language model head
    // struct ggml_tensor * lmh_b; // language model bias

    // key + value memory
    struct ggml_tensor * memory_k;
    struct ggml_tensor * memory_v;

    //
    struct ggml_context * ctx;
    std::map<std::string, struct ggml_tensor *> tensors;
};

// load the model's weights from a file
bool llama_model_load(const std::string & fname, llama_model & model, gpt_vocab & vocab) {
    printf("loading LLaMa from path: '%s' \n", fname.c_str());

    auto fin = std::ifstream(fname, std::ios::binary);
    if (!fin) {
        fprintf(stderr, "%s: failed to open '%s'\n", __func__, fname.c_str());
        return false;
    }

    // verify magic
    {
        uint32_t magic;
        fin.read((char *) &magic, sizeof(magic));
        if (magic != 0x596f7572) { // Your in hex
            fprintf(stderr, "%s: invalid model file '%s' (bad magic)\n", __func__, fname.c_str());
            fprintf(stderr, "Expected 0x596f7572, got 0x%08x", magic);
            return false;
        }
        fin.read((char *) &magic, sizeof(magic));
        if (magic != 0x47505473) {// GPTs in hex
            fprintf(stderr, "%s: invalid model file '%s' (bad magic)\n", __func__, fname.c_str());
            fprintf(stderr, "Expected 0x47505473, got 0x%08x", magic);
            return false;
        }
    }

    // load hparams
    {
        auto & hparams = model.hparams;

        fin.read((char *) &hparams.n_vocab, sizeof(hparams.n_vocab));
        fin.read((char *) &hparams.n_ctx,   sizeof(hparams.n_ctx));
        fin.read((char *) &hparams.n_embd,  sizeof(hparams.n_embd));
        fin.read((char *) &hparams.n_hddn,  sizeof(hparams.n_hddn));
        fin.read((char *) &hparams.n_head,  sizeof(hparams.n_head));
        fin.read((char *) &hparams.n_layer, sizeof(hparams.n_layer));
        fin.read((char *) &hparams.n_rot,   sizeof(hparams.n_rot));
        fin.read((char *) &hparams.f16,     sizeof(hparams.f16));

        // printf("%s: n_vocab = %d\n", __func__, hparams.n_vocab);
        // printf("%s: n_ctx   = %d\n", __func__, hparams.n_ctx);
        // printf("%s: n_embd  = %d\n", __func__, hparams.n_embd);
        // printf("%s: n_hddn  = %d\n", __func__, hparams.n_hddn);
        // printf("%s: n_head  = %d\n", __func__, hparams.n_head);
        // printf("%s: n_layer = %d\n", __func__, hparams.n_layer);
        // printf("%s: n_rot   = %d\n", __func__, hparams.n_rot);
        // printf("%s: f16     = %d\n", __func__, hparams.f16);
    }

    // // load vocab
    // {
    //     int32_t n_vocab = 0;
    //     fin.read((char *) &n_vocab, sizeof(n_vocab));

    //     if (n_vocab != model.hparams.n_vocab) {
    //         fprintf(stderr, "%s: invalid model file '%s' (bad vocab size %d != %d)\n",
    //                 __func__, fname.c_str(), n_vocab, model.hparams.n_vocab);
    //         return false;
    //     }

    //     std::string word;
    //     for (int i = 0; i < n_vocab; i++) {
    //         uint32_t len;
    //         fin.read((char *) &len, sizeof(len));

    //         word.resize(len);
    //         fin.read((char *) word.data(), len);

    //         vocab.token_to_id[word] = i;
    //         vocab.id_to_token[i] = word;
    //     }
    // }

    // for the big tensors, we have the option to store the data in 16-bit floats or quantized
    // in order to save memory and also to speed up the computation
    ggml_type wtype = GGML_TYPE_COUNT;
    switch (model.hparams.f16) {
        case 0: wtype = GGML_TYPE_F32; break;
        case 1: wtype = GGML_TYPE_F16; break;
        case 2: wtype = GGML_TYPE_Q4_0; break;
        case 3: wtype = GGML_TYPE_Q4_1; break;
        default:
            {
                fprintf(stderr, "%s: invalid model file '%s' (bad f16 %d)\n",
                        __func__, fname.c_str(), model.hparams.f16);
                return false;
            }
    }

    const ggml_type wtype2 = GGML_TYPE_F32;

    auto & ctx = model.ctx;

    size_t ctx_size = 0;

    {
        const auto & hparams = model.hparams;

        const int n_embd  = hparams.n_embd;
        const int n_hddn  = hparams.n_hddn;
        const int n_layer = hparams.n_layer;
        const int n_ctx   = hparams.n_ctx;
        const int n_vocab = hparams.n_vocab;

        ctx_size += n_embd * n_vocab * ggml_type_sizef(GGML_TYPE_F16); // wte

        // printf("%s: ggml ctx size embedding = %6.2f MB\n", __func__, ctx_size/(1024.0*1024.0));
        // for each layer
        {
            ctx_size += n_layer * (n_embd * ggml_type_sizef(GGML_TYPE_F32)); // attention_norm

            // printf("%s: ggml ctx size attention_norm = %6.2f MB\n", __func__, ctx_size/(1024.0*1024.0));

            ctx_size += n_layer * (n_embd * n_embd * ggml_type_sizef(wtype)); // c_attn_q_proj_w
            ctx_size += n_layer * (n_embd * n_embd * ggml_type_sizef(wtype)); // c_attn_k_proj_w
            ctx_size += n_layer * (n_embd * n_embd * ggml_type_sizef(wtype)); // c_attn_v_proj_w

            ctx_size += n_layer * (n_embd * n_embd * ggml_type_sizef(wtype)); // c_attn_out_proj_w

            // printf("%s: ggml ctx size c_attn_v_proj_w = %6.2f MB\n", __func__, ctx_size/(1024.0*1024.0));

            ctx_size += n_layer * (n_embd * ggml_type_sizef(GGML_TYPE_F32)); // c_ffn_norm

            // printf("%s: ggml ctx size c_ffn_norm = %6.2f MB\n", __func__, ctx_size/(1024.0*1024.0));
            // ctx_size += n_layer * (n_embd * ggml_type_sizef(GGML_TYPE_F32)); // c_a

            ctx_size += n_layer * (n_hddn * n_embd * ggml_type_sizef(wtype)); // c_feed_forward_w1
            ctx_size += n_layer * (n_hddn * n_embd * ggml_type_sizef(wtype)); // c_feed_forward_w2
            ctx_size += n_layer * (n_embd * n_hddn * ggml_type_sizef(wtype)); // c_feed_forward_w3_trans
        }
        // printf("%s: ggml ctx size w/o final_norm = %6.2f MB\n", __func__, ctx_size/(1024.0*1024.0));

        ctx_size += n_embd * ggml_type_sizef(GGML_TYPE_F32); // final_norm

        ctx_size += n_embd * n_vocab * ggml_type_sizef(wtype); // lmh_g
        // ctx_size += n_vocab * ggml_type_sizef(GGML_TYPE_F32); // lmh_b

        // printf("%s: ggml ctx size w/o memory = %6.2f MB\n", __func__, ctx_size/(1024.0*1024.0));

        ctx_size += n_ctx * n_layer * n_embd * ggml_type_sizef(GGML_TYPE_F32); // memory_k
        ctx_size += n_ctx * n_layer * n_embd * ggml_type_sizef(GGML_TYPE_F32); // memory_v

        ctx_size += (3 + 9 * n_layer) * 256; // object overhead - 3 for wte, final_norm, lmh_g, 9 for each llama layer
        // printf("%s: ggml ctx size w/ memory = %6.2f MB\n", __func__, ctx_size/(1024.0*1024.0));
        // ctx_size += 2; // 8KB for the context itself

        printf("Total model size = %2.6f GB\n", ctx_size/(1024.0*1024.0*1024.0));
    }

    // create the ggml context
    {
        struct ggml_init_params params = {
            .mem_size   = ctx_size,
            .mem_buffer = NULL,
        };

        model.ctx = ggml_init(params);
        if (!model.ctx) {
            fprintf(stderr, "%s: ggml_init() failed\n", __func__);
            return false;
        }
    }

    // prepare memory for the weights
    {
        const auto & hparams = model.hparams;

        const int n_embd  = hparams.n_embd;
        const int n_hddn  = hparams.n_hddn;
        const int n_layer = hparams.n_layer;
        const int n_ctx   = hparams.n_ctx;
        const int n_vocab = hparams.n_vocab;

        model.layers.resize(n_layer);

        model.wte    = ggml_new_tensor_2d(ctx, GGML_TYPE_F16, n_embd, n_vocab);

        model.final_norm = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);
        // model.ln_f_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);

        model.lmh_g  = ggml_new_tensor_2d(ctx, wtype,         n_embd, n_vocab);
        // model.lmh_b  = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_vocab);

        // map by name
        model.tensors["tok_embeddings.weight"] = model.wte;

        model.tensors["norm.weight"] = model.final_norm;
        // model.tensors["transformer.ln_f.bias"]   = model.ln_f_b;

        model.tensors["output.weight"] = model.lmh_g;
        // model.tensors["lm_head.bias"]   = model.lmh_b;

        for (int i = 0; i < n_layer; ++i) {
            auto & layer = model.layers[i];

            layer.attention_norm          = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);
            // layer.ln_1_b                  = ggml_new_tensor_1d(ctx, GGML_TYPE_F32,   n_embd);

            layer.c_attn_q_proj_w         = ggml_new_tensor_2d(ctx, wtype,         n_embd,   n_embd);
            layer.c_attn_k_proj_w         = ggml_new_tensor_2d(ctx, wtype,         n_embd,   n_embd);
            layer.c_attn_v_proj_w         = ggml_new_tensor_2d(ctx, wtype,         n_embd,   n_embd);

            layer.c_attn_proj_w           = ggml_new_tensor_2d(ctx, wtype,         n_embd,   n_embd);

            layer.c_ffn_norm              = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);

            layer.c_feed_forward_w1       = ggml_new_tensor_2d(ctx, wtype,         n_embd,   n_hddn);
            layer.c_feed_forward_w2_trans = ggml_new_tensor_2d(ctx, wtype,         n_hddn,   n_embd);
            layer.c_feed_forward_w3       = ggml_new_tensor_2d(ctx, wtype,         n_embd,   n_hddn);

            // map by name
            model.tensors["layers." + std::to_string(i) + ".attention_norm.weight"] = layer.attention_norm;

            model.tensors["layers." + std::to_string(i) + ".attention.wq.weight"]   = layer.c_attn_q_proj_w;
            model.tensors["layers." + std::to_string(i) + ".attention.wk.weight"]   = layer.c_attn_k_proj_w;
            model.tensors["layers." + std::to_string(i) + ".attention.wv.weight"]   = layer.c_attn_v_proj_w;
            model.tensors["layers." + std::to_string(i) + ".attention.wo.weight"]   = layer.c_attn_proj_w;

            model.tensors["layers." + std::to_string(i) + ".ffn_norm.weight"]   = layer.c_ffn_norm;

            model.tensors["layers." + std::to_string(i) + ".feed_forward.w1.weight"] = layer.c_feed_forward_w1;
            model.tensors["layers." + std::to_string(i) + ".feed_forward.w2.weight"] = layer.c_feed_forward_w2_trans;
            model.tensors["layers." + std::to_string(i) + ".feed_forward.w3.weight"] = layer.c_feed_forward_w3;
        }
    }

    // key + value memory
    {
        const auto & hparams = model.hparams;

        const int n_embd  = hparams.n_embd;
        const int n_layer = hparams.n_layer;
        const int n_ctx   = hparams.n_ctx;

        const int n_mem      = n_layer*n_ctx;
        const int n_elements = n_embd*n_mem;

        model.memory_k = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_elements);
        model.memory_v = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_elements);

        const size_t memory_size = ggml_nbytes(model.memory_k) + ggml_nbytes(model.memory_v);

        // printf("%s: memory_size = %8.2f MB, n_mem = %d\n", __func__, memory_size/1024.0/1024.0, n_mem);
    }

    // load weights
    {
        int n_tensors = 0;
        size_t total_size = 0;

        // printf("%s: ", __func__);

        while (true) {
            int32_t n_dims;
            int32_t length;
            int32_t ftype;

            fin.read(reinterpret_cast<char *>(&n_dims), sizeof(n_dims));
            fin.read(reinterpret_cast<char *>(&length), sizeof(length));
            fin.read(reinterpret_cast<char *>(&ftype),  sizeof(ftype));

            if (fin.eof()) {
                break;
            }

            int32_t nelements = 1;
            int32_t ne[2] = { 1, 1 };
            for (int i = 0; i < n_dims; ++i) {
                fin.read(reinterpret_cast<char *>(&ne[i]), sizeof(ne[i]));
                nelements *= ne[i];
            }

            std::string name(length, 0);
            fin.read(&name[0], length);

            if (model.tensors.find(name.data()) == model.tensors.end()) {
                fprintf(stderr, "%s: unknown tensor '%s' in model file\n", __func__, name.data());
                return false;
            }

            auto tensor = model.tensors[name.data()];
            if (ggml_nelements(tensor) != nelements) {
                fprintf(stderr, "%s: tensor '%s' has wrong size in model file\n", __func__, name.data());
                return false;
            }

            if (tensor->ne[0] != ne[0] || tensor->ne[1] != ne[1]) {
                fprintf(stderr, "%s: tensor '%s' has wrong shape in model file: got [%d, %d], expected [%d, %d]\n",
                        __func__, name.data(), tensor->ne[0], tensor->ne[1], ne[0], ne[1]);
                return false;
            }

            if (0) {
                static const char * ftype_str[] = { "f32", "f16", "q4_0", "q4_1", };
                printf("%24s - [%5d, %5d], type = %6s, %6.2f MB\n, %9zu bytes\n",
                    name.data(), ne[0], ne[1], ftype_str[ftype], ggml_nbytes(tensor)/1024.0/1024.0, ggml_nbytes(tensor));
            }
        
            size_t bpe = 0;

            switch (ftype) {
                case 0: bpe = ggml_type_size(GGML_TYPE_F32); break;
                case 1: bpe = ggml_type_size(GGML_TYPE_F16); break;
                case 2: bpe = ggml_type_size(GGML_TYPE_Q4_0); assert(ne[0] % 64 == 0); break;
                case 3: bpe = ggml_type_size(GGML_TYPE_Q4_1); assert(ne[0] % 64 == 0); break;
                default: {
                    fprintf(stderr, "%s: unknown ftype %d in model file\n", __func__, ftype);
                    return false;
                }
            };

            if (nelements*bpe/ggml_blck_size(tensor->type) != ggml_nbytes(tensor)) {
                fprintf(stderr, "%s: tensor '%s' has wrong size in model file: got %zu, expected %zu\n",
                        __func__, name.data(), ggml_nbytes(tensor), nelements*bpe);
                fprintf(stderr, "  nelements = %d, bpe = %zu, ftype = %d", nelements, bpe, ftype);
                return false;
            }

            fin.read(reinterpret_cast<char *>(tensor->data), ggml_nbytes(tensor));
            // If tensor name is "tok_embeddings.weight", then print the first 10 elements (it is float16, typedef __fp16 ggml_fp16_t, so we need to cast to float and print 7 digits after the decimal point)
            if ( name == "layers.0.attention_norm.weight") {
                printf("First 10 elements of layers.0.attention_norm.weight (of size: %zu): ", nelements*bpe/4);
                for (int i = 0; i < 5; i++) {
                    // printf("%.9f ", (float)((ggml_fp16_t *)tensor->data)[i]);
                    printf("%.4f ", ((float *)tensor->data)[i]);
                } // similarly for last 5 elements
                for (int i = nelements-5; i < nelements; i++) {
                    printf("%.4f ", ((float *)tensor->data)[i]);
                }
                printf("\n");
            }

            // printf("%42s - [%5d, %5d], type = %6s, %6.2f MB\n", name.data(), ne[0], ne[1], ftype == 0 ? "float" : "f16", ggml_nbytes(tensor)/1024.0/1024.0);
            total_size += ggml_nbytes(tensor);
            if (++n_tensors % 64 == 0) {
                printf("Still Loading ...\n");
                fflush(stdout);
            }
        }

        printf("Finished loading LLaMa\n");

        // printf("%s: model size = %8.2f MB / num tensors = %d\n", __func__, total_size/1024.0/1024.0, n_tensors);
    }

    fin.close();

    return true;
}

// evaluate the transformer
//
//   - model:     the model
//   - n_threads: number of threads to use
//   - n_past:    the context size so far
//   - embd_inp:  the embeddings of the tokens in the context
//   - embd_w:    the predicted logits for the next token
//
// The GPT-J model requires about 16MB of memory per input token.
//
bool llama_eval(
        const llama_model & model,
        const int n_threads,
        const int n_past,
        const std::vector<gpt_vocab::id> & embd_inp,
              std::vector<float>         & embd_w,
              size_t                     & mem_per_token) {
    const int N = embd_inp.size();

    const auto & hparams = model.hparams;

    const int n_embd  = hparams.n_embd;
    const int n_hddn  = hparams.n_hddn;
    const int n_layer = hparams.n_layer;
    const int n_ctx   = hparams.n_ctx;
    const int n_head  = hparams.n_head;
    const int n_vocab = hparams.n_vocab;
    const int n_rot   = hparams.n_rot;

    const int d_key = n_embd / n_head;

    static size_t buf_size = 256u * 1024 * 1024;
    static void * buf = malloc(buf_size);

    if (mem_per_token > 0 && mem_per_token * N > buf_size) {
        const size_t buf_size_new = 1.1 * (mem_per_token * N); // add 10% to account for ggml object overhead
        //printf("\n%s: reallocating buffer from %zu to %zu bytes\n", __func__, buf_size, buf_size_new);

        // reallocate
        buf_size = buf_size_new;
        buf = realloc(buf, buf_size);
        if (buf == nullptr) {
            fprintf(stderr, "%s: failed to allocate %zu bytes\n", __func__, buf_size);
            return false;
        }
    }

    struct ggml_init_params params = {
        .mem_size   = buf_size,
        .mem_buffer = buf,
    };

    struct ggml_context * ctx0 = ggml_init(params);
    struct ggml_cgraph gf = { .n_threads = n_threads };

    struct ggml_tensor * embd = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, N);
    memcpy(embd->data, embd_inp.data(), N*ggml_element_size(embd));

    // wte
    struct ggml_tensor * inpL = ggml_get_rows(ctx0, model.wte, embd);

    for (int il = 0; il < n_layer; ++il) {
        struct ggml_tensor * cur;

        // attention norm and pass it through the attention layers then residual add.
        {
            cur = ggml_rms_norm(ctx0, inpL);

            // cur = cur * attention_norm
            cur = ggml_mul(
                ctx0, ggml_repeat(ctx0, model.layers[il].attention_norm, cur), cur);
        }

        // self-attention
        {
            struct ggml_tensor * Qcur = ggml_mul_mat(ctx0, model.layers[il].c_attn_q_proj_w, cur);
            struct ggml_tensor * Kcur = ggml_mul_mat(ctx0, model.layers[il].c_attn_k_proj_w, cur);
            struct ggml_tensor * Vcur = ggml_mul_mat(ctx0, model.layers[il].c_attn_v_proj_w, cur);

            // store key and value to memory
            if (N >= 1) {
                struct ggml_tensor * k = ggml_view_1d(ctx0, model.memory_k, N*n_embd, (ggml_element_size(model.memory_k)*n_embd)*(il*n_ctx + n_past));
                struct ggml_tensor * v = ggml_view_1d(ctx0, model.memory_v, N*n_embd, (ggml_element_size(model.memory_v)*n_embd)*(il*n_ctx + n_past));

                ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Kcur, k));
                ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Vcur, v));
            }

            // Q = Qcur.contiguous().view(n_embd/n_head, n_head, N).permute(0, 2, 1, 3)
            struct ggml_tensor * Q =
                ggml_permute(ctx0,
                        ggml_rope(ctx0,
                            ggml_cpy(ctx0,
                                Qcur,
                                ggml_new_tensor_3d(ctx0, GGML_TYPE_F32, n_embd/n_head, n_head, N)),
                            n_past, n_rot, 0, 1),
                        0, 2, 1, 3);

            // K = Kmem.view(n_embd/n_head, n_head, n_past + N).permute(0, 2, 1, 3)
            struct ggml_tensor * K =
                ggml_permute(ctx0,
                        ggml_rope(ctx0,
                            ggml_reshape_3d(ctx0,
                                ggml_view_1d(ctx0, model.memory_k, (n_past + N)*n_embd, il*n_ctx*ggml_element_size(model.memory_k)*n_embd),
                                n_embd/n_head, n_head, n_past + N),
                            n_past, n_rot, 1, 1),
                        0, 2, 1, 3);

            // K * Q
            struct ggml_tensor * KQ = ggml_mul_mat(ctx0, K, Q);

            // KQ_scaled = KQ / sqrt(n_embd/n_head)
            struct ggml_tensor * KQ_scaled =
                ggml_scale(ctx0,
                        KQ,
                        ggml_new_f32(ctx0, 1.0f/sqrt(float(n_embd)/n_head))
                        );

            // KQ_masked = mask_past(KQ_scaled)
            struct ggml_tensor * KQ_masked = ggml_diag_mask_inf(ctx0, KQ_scaled, n_past);

            // KQ = soft_max(KQ_masked)
            struct ggml_tensor * KQ_soft_max = ggml_soft_max(ctx0, KQ_masked);

            // V_trans = Vmem.view(n_embd/n_head, n_head, n_past + N).permute(1, 2, 0, 3).contiguous()
            struct ggml_tensor * V_trans =
                ggml_permute(ctx0,
                        ggml_reshape_3d(ctx0,
                            ggml_view_1d(ctx0, model.memory_v, (n_past + N)*n_embd, il*n_ctx*ggml_element_size(model.memory_v)*n_embd),
                            n_embd/n_head, n_head, n_past + N),
                        1, 2, 0, 3);

            // KQV = transpose(V) * KQ_soft_max
            struct ggml_tensor * KQV = ggml_mul_mat(ctx0, V_trans, KQ_soft_max);

            // KQV_merged = KQV.permute(0, 2, 1, 3)
            struct ggml_tensor * KQV_merged = ggml_permute(ctx0, KQV, 0, 2, 1, 3);

            // cur = KQV_merged.contiguous().view(n_embd, N)
            cur = ggml_cpy(ctx0,
                    KQV_merged,
                    ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, n_embd, N));

            // projection (no bias)
            cur = ggml_mul_mat(ctx0, model.layers[il].c_attn_proj_w, cur);
        }

        // self-attention + Input
        inpL = ggml_add(ctx0, inpL, cur);

        // ffn norm and pass it through the ff layers then residual add
        {
            cur = ggml_rms_norm(ctx0, inpL);

            // cur = cur * c_ffn_norm
            cur = ggml_mul(
                ctx0, ggml_repeat(ctx0, model.layers[il].c_ffn_norm, cur), cur);
        }

        // TODO: Get malloc right for cur
        // feed-forward network
        {
            // note here we pass inpL (residual added) instead of cur
            struct ggml_tensor * ff1_out = ggml_mul_mat(ctx0, model.layers[il].c_feed_forward_w1, cur);

            struct ggml_tensor * ff3_out = ggml_mul_mat(ctx0, model.layers[il].c_feed_forward_w3, cur);

            // SiLU activation
            ff1_out = ggml_silu(ctx0, ff1_out);

            // Do gating via ff3_out
            cur = ggml_mul(ctx0, ff1_out, ff3_out);

            // projection
            // cur = matmil(proj_w, cur)
            cur = ggml_mul_mat(ctx0, model.layers[il].c_feed_forward_w2_trans, cur);
        }

        // input for next layer
        inpL = ggml_add(ctx0, cur, inpL);
    }

    // final norm
    {
        inpL = ggml_rms_norm(ctx0, inpL);

        // inpL = inpL * final_norm
        inpL = ggml_mul(
            ctx0, ggml_repeat(ctx0, model.final_norm, inpL), inpL);
    }

    // lm_head
    {
        inpL = ggml_mul_mat(ctx0, model.lmh_g, inpL);
    }

    // logits -> probs
    //inpL = ggml_soft_max(ctx0, inpL);

    // run the computation
    ggml_build_forward_expand(&gf, inpL);
    ggml_graph_compute       (ctx0, &gf);

    // if (n_past%100 == 0) {
    //    ggml_graph_print   (&gf);
    //    ggml_graph_dump_dot(&gf, NULL, "llama.dot");
    // }
    // return true;


    //embd_w.resize(n_vocab*N);
    //memcpy(embd_w.data(), ggml_get_data(inpL), sizeof(float)*n_vocab*N);

    // return result for just the last token
    embd_w.resize(n_vocab);
    memcpy(embd_w.data(), (float *) ggml_get_data(inpL) + (n_vocab*(N-1)), sizeof(float)*n_vocab);

    if (mem_per_token == 0) {
        mem_per_token = ggml_used_mem(ctx0)/N;
    }
    //printf("used_mem = %zu\n", ggml_used_mem(ctx0));

    ggml_free(ctx0);

    return true;
}

bool llama_vocab_load(std::string vocab_path, int vocab_size, gpt_vocab & vocab) {
    // Open file for reading in binary mode
    std::ifstream infile(vocab_path);

    // if file is not opened, return false
    if (!infile.is_open()) {
        return false;
    }

    // Read file line by line and update the vocab
    std::string line;
    int i = 0;
    while (std::getline(infile, line)) {
        vocab.token_to_id[line] = i;
        vocab.id_to_token[i] = line;
        i++;
    }

    std::cout << "Words read from file:" << std::endl;

    // print the size of the vocab using std::wcout
    std::cout << "vocab size = " << vocab.token_to_id.size() << std::endl;
    // print the first 3 tokens and last 3 tokens with their ids
    for (int i = 0; i < 3; i++) {
        std::cout << i << ": " << vocab.id_to_token[i] << ", ";
    }
    std::cout << "..." ;
    for (int i = vocab_size - 3; i < vocab_size; i++) {
        std::cout << i << " " << vocab.id_to_token[i] << ", ";
    }
    std::cout << std::endl;

    return true;
}


int main(int argc, char ** argv) {
    const int64_t t_main_start_us = ggml_time_us();

    gpt_params params;
    params.model = "models/llama-model.bin";
    std::vector<gpt_vocab::id> embd_inp;
    // if argc == 1, then we are running as a subprocess
    // print argc
    printf("%d\n", argc);
    if (argc < 2) {
        // Set the seed, n_threads, n_predict, top_k, top_p, temp, model for params
        // Read from stdin each of params attributes
        // The attributes will be in same order as above, separated by newlines.
        // The first line will be the seed
        std::string input;
        std::getline(std::cin, input);
        params.seed = std::stoi(input);
        // The second line will be the number of threads
        std::getline(std::cin, input);
        params.n_threads = std::stoi(input);
        // The third line will be the number of predictions
        std::getline(std::cin, input);
        params.n_predict = std::stoi(input);
        // The fourth line will be the top_k
        std::getline(std::cin, input);
        params.top_k = std::stoi(input);
        // The fifth line will be the top_p (float)
        std::getline(std::cin, input);
        params.top_p = std::stof(input);
        // The sixth line will be the temperature (float)
        std::getline(std::cin, input);
        params.temp = std::stof(input);
        // The seventh line will be the model path (string)
        std::getline(std::cin, input);
        params.model = input;

        // Next will be the number of tokens in the prompt
        std::getline(std::cin, input);
        int num_tokens = std::stoi(input);
        // Next `num_tokens` lines will be the prompt tokens
        for (int i = 0; i < num_tokens; i++) {
            std::getline(std::cin, input);
            embd_inp.push_back(std::stoi(input));
        }
        // Next will be last message, containing a string "END"
        std::getline(std::cin, input);
        // sanity check
        if (input != "END") {
            std::cerr << "ERROR: Expected END, got " << input << std::endl;
            return 1;
        }
        // print all params
        printf("Seed: %d\n", params.seed);
        printf("n_threads: %d\n", params.n_threads);
        printf("n_predict: %d\n", params.n_predict);
        printf("top_k: %d\n", params.top_k);
        printf("top_p: %f\n", params.top_p);
        printf("temp: %f\n", params.temp);
        printf("model: %s\n", params.model.c_str());
        printf("prompta: ");
        for (int i = 0; i < embd_inp.size(); i++) {
            printf("%d ", embd_inp[i]);
        }
        printf("\n");
    } else {
        printf("prompt: ");
        if (gpt_params_parse(argc, argv, params) == false || params.temp <= 0.0f) {
            printf("Either invalid parameters of temperature <= 0.0f");
            return 1;
        }
        // Read the prompt token ids from stdin, separated by whitespace
        std::string input;
        std::getline(std::cin, input);
        std::stringstream ss(input);
        int num;
        while (ss >> num) {
            embd_inp.push_back(num);
        }

        // embd_inp = {1,   887,   526,   302,  8842, 29889};
    }


    if (params.seed < 0) {
        params.seed = time(NULL);
    }

    printf("%s: seed = %d\n", __func__, params.seed);

    std::mt19937 rng(params.seed);
    if (params.prompt.empty()) {
        params.prompt = gpt_random_prompt(rng);
    }

    int64_t t_load_us = 0;

    gpt_vocab vocab;
    llama_model model;

    // load the model
    {
        const int64_t t_start_us = ggml_time_us();

        if (!llama_model_load(params.model, model, vocab)) {
            fprintf(stderr, "%s: failed to load model from '%s'\n", __func__, params.model.c_str());
            return 1;
        }

        t_load_us = ggml_time_us() - t_start_us;
    }

    // load the vocab
    {
        const int64_t t_start_us = ggml_time_us();

        if (!llama_vocab_load(params.vocab_path, model.hparams.n_vocab, vocab)) {
            fprintf(stderr, "%s: failed to load vocab from '%s'\n", __func__, params.model.c_str());
            return 1;
        } else {
            printf("Loaded vocab from %s\n", params.vocab_path.c_str());
        }

        t_load_us += ggml_time_us() - t_start_us;
    }

    int n_past = 0;

    int64_t t_sample_us  = 0;
    int64_t t_predict_us = 0;

    std::vector<float> logits;

    // tokenize the prompt
    // TODO: Add tokenizer support
    // std::vector<gpt_vocab::id> embd_inp = ::gpt_tokenize(vocab, params.prompt);
    // For now we initialize embd_inp with the numbers [1, 822, 6088, 29918, 1420, 29898]

    // type of gpt_vocab::id is int32_t, we print all the embd_inp
    printf("embd_inp: ");
    for (int i = 0; i < embd_inp.size(); i++) {
        printf("%d ", embd_inp[i]);
    }
    printf("\n");
    params.n_predict = std::min(params.n_predict, model.hparams.n_ctx - (int) embd_inp.size());
    printf("%s: number of tokens in prompt = %zu\n", __func__, embd_inp.size());
    printf("\n");
    // print first 5 elements of (void *) model.wte->data
    // printf("model.wte->data: ");
    // for (int i = 0; i < 5; i++) {
    //    printf("%d ", ((int *) model.wte->data)[i]);
    //}
    printf("\n------------------ Starting Generation -----------------\n");

    std::vector<gpt_vocab::id> embd;

    // determine the required inference memory per token:
    size_t mem_per_token = 0;
    llama_eval(model, params.n_threads, 0, {1,   887,   526,   302,  29889}, logits, mem_per_token);
    printf("\n\n\n\n");
    int iiii = 0;
    for (int i = embd.size(); i < embd_inp.size() + params.n_predict; i++) {
        // predict
        if (embd.size() > 0) {
            const int64_t t_start_us = ggml_time_us();
            // printf("\nembd: ");
            // for (int i = 0; i < embd.size(); i++) {
            //     printf("%d ", embd[i]);
            // }
            // printf("\n");
            if (!llama_eval(model, params.n_threads, n_past, embd, logits, mem_per_token)) {
                printf("Failed to predict\n");
                return 1;
            }
            // printf("%d logits: ", iiii++);
            // for (int i = 0; i < logits.size(); i++) {
            //     if (i < 5 || i > logits.size() - 5) {
            //         printf("%.7f ", logits[i]);
            //     } else if (i == 5) {
            //         printf("... ");
            //     }
            // }
            // printf("\n");

            t_predict_us += ggml_time_us() - t_start_us;
        }

        n_past += embd.size();
        embd.clear();

        if (i >= embd_inp.size()) {
            // sample next token
            const int   top_k = params.top_k;
            const float top_p = params.top_p;
            const float temp  = params.temp;

            const int n_vocab = model.hparams.n_vocab;
            gpt_vocab::id id = 0;

            {
                const int64_t t_start_sample_us = ggml_time_us();

                // Take the index of logit with the highest value
                id = 0;
                float max_value = logits[0];
                // printf("logits size : %d", logits.size());
                // printf(" first 5 logits: %f %f %f %f %f", logits[0], logits[1], logits[2], logits[3], logits[4]);

                // for (int i = 1; i < logits.size(); i++) {
                //     if (logits[i] > max_value) {
                //         // printf("\nAt %d, the logit is %.6f", i, logits[i]);
                //         max_value = logits[i];
                //         id = i;
                //     }
                // }
                // printf("\nAt 1485, the logit is %.6f", logits[1485]);
                // printf(" max: %.6f, id: %d \n", max_value, id);
                id = gpt_sample_top_k_top_p(vocab, logits.data() + (logits.size() - n_vocab), top_k, top_p, temp, rng);

                t_sample_us += ggml_time_us() - t_start_sample_us;
            }

            // add it to the context
            embd.push_back(id);
        } else {
            // if here, it means we are still processing the input prompt
            for (int k = i; k < embd_inp.size(); k++) {
                embd.push_back(embd_inp[k]);
                if (embd.size() > params.n_batch) {
                    break;
                }
            }
            i += embd.size() - 1;
        }

        // display text
        for (auto id : embd) {
            // Replace `<0x0A>` vocab.id_to_token[id].c_str() with newline
            if (id == 13) {
                printf("\n");
            } else {
                // Replace all `▁` in vocab.id_to_token[id].c_str() with space
                // std::string token = ];
                // std::replace(token.begin(), token.end(), "\u2581", ' ');
                printf("%s", vocab.id_to_token[id].c_str());
            }
            // printf("%d", id);
        }
        fflush(stdout);

        // end of text token
        if (embd.back() == 50256) {
            break;
        }
    }

    // // report timing
    // {
    //     const int64_t t_main_end_us = ggml_time_us();

    //     printf("\n\n");
    //     printf("%s: mem per token = %8zu bytes\n", __func__, mem_per_token);
    //     printf("%s:     load time = %8.2f ms\n", __func__, t_load_us/1000.0f);
    //     printf("%s:   sample time = %8.2f ms\n", __func__, t_sample_us/1000.0f);
    //     printf("%s:  predict time = %8.2f ms / %.2f ms per token\n", __func__, t_predict_us/1000.0f, t_predict_us/1000.0f/n_past);
    //     printf("%s:    total time = %8.2f ms\n", __func__, (t_main_end_us - t_main_start_us)/1000.0f);
    // }

    ggml_free(model.ctx);

    return 0;
}

// extern "C" {
//     int call_main(int seed, int threads, int n_predict, int top_k, float top_p, float temp, char * model_path,
//                 int input_length, int * input_tokens, message_callback callback) {
//         const int64_t t_main_start_us = ggml_time_us();

//         // Set the seed, n_threads, n_predict, top_k, top_p, temp, model for params
//         gpt_params params;
//         params.seed = seed;
//         params.n_threads = threads;
//         params.n_predict = n_predict;
//         params.top_k = top_k;
//         params.top_p = top_p;
//         params.temp = temp;
//         params.model = model_path;

//         // // params.model = "models/llama-model.bin";
//         // // We fill argv_pointer (space separated) into argv
//         // // message callback the arguments
//         // for (int i = 0; i < _argc; i++) {
//         //     callback(argv[i]);
//         // }

//         // if (gpt_params_parse(argc, argv, params) == false) {
//         //     return 1;
//         // }

//         if (params.seed < 0) {
//             params.seed = time(NULL);
//         }

//         printf("%s: seed = %d\n", __func__, params.seed);

//         std::mt19937 rng(params.seed);
//         if (params.prompt.empty()) {
//             params.prompt = gpt_random_prompt(rng);
//         }

//         int64_t t_load_us = 0;

//         gpt_vocab vocab;
//         llama_model model;

//         // load the model
//         {
//             const int64_t t_start_us = ggml_time_us();

//             if (!llama_model_load(params.model, model, vocab)) {
//                 fprintf(stderr, "%s: failed to load model from '%s'\n", __func__, params.model.c_str());
//                 return 1;
//             }

//             t_load_us = ggml_time_us() - t_start_us;
//         }

//         int n_past = 0;

//         int64_t t_sample_us  = 0;
//         int64_t t_predict_us = 0;

//         std::vector<float> logits;

//         // tokenize the prompt
//         // TODO: Add tokenizer support
//         // std::vector<gpt_vocab::id> embd_inp = ::gpt_tokenize(vocab, params.prompt);
//         // For now we initialize embd_inp with the numbers [1, 822, 6088, 29918, 1420, 29898]
//         // std::vector<gpt_vocab::id> embd_inp = {1, 822, 6088, 29918, 1420, 29898};
//         // We populate std::vector<gpt_vocab::id> embd_inp with the numbers from the input_tokens array of length input_length
//         std::vector<gpt_vocab::id> embd_inp;
//         for (int i = 0; i < input_length; i++) {
//             embd_inp.push_back(input_tokens[i]);
//         }

//         // type of gpt_vocab::id is int32_t, we print all the embd_inp
//         printf("embd_inp: ");
//         for (int i = 0; i < embd_inp.size(); i++) {
//             printf("%d ", embd_inp[i]);
//         }

//         params.n_predict = std::min(params.n_predict, model.hparams.n_ctx - (int) embd_inp.size());

//         printf("%s: number of tokens in prompt = %zu\n", __func__, embd_inp.size());
//         printf("\n");

//         std::vector<gpt_vocab::id> embd;

//         // determine the required inference memory per token:
//         size_t mem_per_token = 0;
//         llama_eval(model, params.n_threads, 0, { 0, 1, 2, 3 }, logits, mem_per_token);
//         int iiii = 0;
//         for (int i = embd.size(); i < embd_inp.size() + params.n_predict; i++) {
//             // predict
//             if (embd.size() > 0) {
//                 const int64_t t_start_us = ggml_time_us();

//                 if (!llama_eval(model, params.n_threads, n_past, embd, logits, mem_per_token)) {
//                     printf("Failed to predict\n");
//                     return 1;
//                 }
//                 // printf("%d logits: ", iiii++);
//                 // for (int i = 0; i < 5; i++) {
//                 //     printf("%.3f ", logits[i]);
//                 //     if (i == 10) {
//                 //         break;
//                 //     }
//                 // }
//                 // printf("\n");
//                 t_predict_us += ggml_time_us() - t_start_us;
//             }

//             n_past += embd.size();
//             embd.clear();

//             if (i >= embd_inp.size()) {
//                 // sample next token
//                 const int   top_k = params.top_k;
//                 const float top_p = params.top_p;
//                 const float temp  = params.temp;

//                 const int n_vocab = model.hparams.n_vocab;
//                 gpt_vocab::id id = 0;

//                 {
//                     const int64_t t_start_sample_us = ggml_time_us();

//                     // Take the index of logit with the highest value
//                     id = 0;
//                     float max_value = logits[0];
//                     // printf("logits size : %d", logits.size());
//                     // printf("first 5 logits: %f %f %f %f %f", logits[0], logits[1], logits[2], logits[3], logits[4]);

//                     for (int i = 1; i < logits.size(); i++) {
//                         if (logits[i] > max_value) {
//                             max_value = logits[i];
//                             id = i;
//                         }
//                     }
//                     // printf("max: %.3f, id: %d \n", max_value, id);
//                     // id = gpt_sample_top_k_top_p(vocab, logits.data() + (logits.size() - n_vocab), top_k, top_p, temp, rng);

//                     t_sample_us += ggml_time_us() - t_start_sample_us;
//                 }

//                 // add it to the context
//                 embd.push_back(id);
//             } else {
//                 // if here, it means we are still processing the input prompt
//                 for (int k = i; k < embd_inp.size(); k++) {
//                     embd.push_back(embd_inp[k]);
//                     if (embd.size() > params.n_batch) {
//                         break;
//                     }
//                 }
//                 i += embd.size() - 1;
//             }

//             // display text
//             // for (auto id : embd) {
//             //     char message[100];
//             //     snprintf(message, 100, "%d ", id);
//             //     callback(message);
//             //     // printf("%d ", id);
//             //     // printf("%s", vocab.id_to_token[id].c_str());
//             // }
//             fflush(stdout);

//             // end of text token
//             if (embd.back() == 50256) {
//                 break;
//             }
//         }

//         // report timing
//         {
//             const int64_t t_main_end_us = ggml_time_us();

//             printf("\n\n");
//             printf("%s: mem per token = %8zu bytes\n", __func__, mem_per_token);
//             printf("%s:     load time = %8.2f ms\n", __func__, t_load_us/1000.0f);
//             printf("%s:   sample time = %8.2f ms\n", __func__, t_sample_us/1000.0f);
//             printf("%s:  predict time = %8.2f ms / %.2f ms per token\n", __func__, t_predict_us/1000.0f, t_predict_us/1000.0f/n_past);
//             printf("%s:    total time = %8.2f ms\n", __func__, (t_main_end_us - t_main_start_us)/1000.0f);
//         }

//         ggml_free(model.ctx);

//         return 0;
//         // return main(argc, argv);
//     }
// }
