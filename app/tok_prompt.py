import sentencepiece

sp_model = sentencepiece.SentencePieceProcessor(
    model_file='../../llama/save/tokenizer.model')

prompt = """Following is a function in python that takes a list of integers as input and returns the sum of all even numbers in the list.

def sum_even_numbers(list_of_numbers)"""

tokens = [sp_model.bos_id()] + sp_model.encode(prompt)
print(" ".join(str(x) for x in tokens))
