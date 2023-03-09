# import ctypes
# import threading

# # Define the callback function that will receive messages from the C function
# def message_callback(message):
#     print("Received message:", message)

# # Load the shared library
# llama_lib = ctypes.CDLL('../build/my_libs/libllama-lib.dylib')

# # Get the function by name and specify its argument types and return type
# call_main_func = llama_lib.call_main
# # int call_main(int seed, int threads, int n_predict, int top_k, float top_p, float temp,
# #       char * model_path, int input_length, int * input_tokens, message_callback callback)
# call_main_func.argtypes = (ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_float,
#                            ctypes.c_float, ctypes.c_char_p, ctypes.c_int,
#                            ctypes.POINTER(ctypes.c_int), ctypes.c_void_p)
# call_main_func.restype = ctypes.c_int

# # Wrap the Python function with a CFunctionType instance
# message_callback_c = ctypes.CFUNCTYPE(None, ctypes.c_char_p)(message_callback)

# # convert c_char_p to zero terminated string

# # Pass the callback function pointer to the add function
# # Input arguments are passed as a tuple

# seed, threads, n_predict, top_k, top_p, temp = 0, 1, 100, 10, 0.9, 1.0
# model_path, input_length = b"../models/llama-q4_0.bin" + b"\0", 6
# input_tokens = [1, 822, 6088, 29918, 1420, 29898]
# # convert input_tokens to c_int array
# input_tokens_c = (ctypes.c_int * len(input_tokens))(*input_tokens)

# call_main_args = (seed, threads, n_predict, top_k, top_p, temp, model_path, input_length,
#                   input_tokens_c, message_callback_c)

# # Create a new thread to call the add function
# call_main_thread = threading.Thread(target=call_main_func, args=call_main_args)

# # Start the thread
# call_main_thread.start()

import subprocess
import os
import time

# Launch the C++ binary as a subprocess
function_process = subprocess.Popen(['sudo nice -n -20 ../build/bin/llama'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
# os.nice(-10)
# Send input to the C++ binary and receive output
input_value = 10
seed, threads, n_predict, top_k, top_p, temp = 0, 1, 100, 10, 0.9, 1.0
model_path, input_length = "../models/llama-q4_0.bin", 6

# seed
function_process.stdin.write(str(seed).encode('utf-8') + os.linesep.encode('utf-8'))
function_process.stdin.flush()

# threads
function_process.stdin.write(str(threads).encode('utf-8') + os.linesep.encode('utf-8'))
function_process.stdin.flush()

# n_predict
function_process.stdin.write(str(n_predict).encode('utf-8') + os.linesep.encode('utf-8'))
function_process.stdin.flush()

# top_k
function_process.stdin.write(str(top_k).encode('utf-8') + os.linesep.encode('utf-8'))
function_process.stdin.flush()

# top_p
function_process.stdin.write(str(top_p).encode('utf-8') + os.linesep.encode('utf-8'))
function_process.stdin.flush()

# temp
function_process.stdin.write(str(temp).encode('utf-8') + os.linesep.encode('utf-8'))
function_process.stdin.flush()

# model_path
function_process.stdin.write(model_path.encode('utf-8') + os.linesep.encode('utf-8'))
function_process.stdin.flush()

# input_length
function_process.stdin.write(str(input_length).encode('utf-8') + os.linesep.encode('utf-8'))
function_process.stdin.flush()

# `input_tokens` tokens
for token in [1, 822, 6088, 29918, 1420, 29898]:
    function_process.stdin.write(str(token).encode('utf-8') + os.linesep.encode('utf-8'))
    function_process.stdin.flush()
# "END"
function_process.stdin.write(b"END\n")
function_process.stdin.flush()


while True:
    output = function_process.stdout.readline().decode('utf-8').rstrip()
    if len(output) != 0:
        print('Python:',output)
    else:
        time.sleep(1)




# # Send input to the C++ binary again and receive output
# input_value = 20
# function_process.stdin.write(str(input_value).encode('utf-8') + os.linesep.encode('utf-8'))
# function_process.stdin.flush()
# output = function_process.stdout.readline().decode('utf-8').rstrip()
# print(output)

# # Send "quit" command to the C++ binary to terminate it
# function_process.stdin.write(b"quit\n")
# function_process.stdin.flush()

# # Wait for the subprocess to finish
# function_process.wait()

