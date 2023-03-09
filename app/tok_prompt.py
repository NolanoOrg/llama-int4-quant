import sentencepiece

sp_model = sentencepiece.SentencePieceProcessor(
    model_file='../models/tokenizer.model')

# prompt = """We describe a system called Overton, whose main design goal is to support engineers in building, monitoring, and improving production  machinelearning systems. Key challenges engineers face are monitoring fine-grained quality, diagnosing errors in sophisticated applications, and  handling contradictory or incomplete supervision data. Overton automates the life cycle of model construction, deployment, and monitoring by providing a set of novel high-level, declarative abstractions. Overton's"""
prompt = """Following is a function in python that takes a list of integers as input and returns the sum of all even numbers in the list.

def sum_even_numbers(list_of_numbers)"""
# prompt = """We describe a system called Overton, whose main design goal is to support engineers in building, monitoring, and improving production  machinelearning systems. Key challenges engineers face are monitoring fine-grained quality, diagnosing errors in sophisticated applications, and  handling contradictory or incomplete supervision data."""
# prompt = """You are a high school senior who has just been accepted to a prestigious university, but you are worried about the cost of tuition. Write an email to the university's scholarship office explaining your financial situation and why you would be a good candidate for a scholarship. Start by introducing yourself and thanking the scholarship office for their time. Then explain your financial situation and why you need a scholarship to attend the university. You could mention any financial hardships your family has faced, such as job loss or medical expenses. Next, highlight your academic achievements and extracurricular activities. \n If you have a high GPA or test scores, mention them. If you have participated in clubs or sports, explain how they have helped you develop leadership skills or a passion for a particular subject. \n Be sure to proofread your email before sending it, and make sure you have included all relevant information. Good luck! \n Dear Prof."""

tokens = [sp_model.bos_id()] + sp_model.encode(prompt)
print(" ".join(str(x) for x in tokens))
