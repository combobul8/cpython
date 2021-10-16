import custom
import time

mycustom = custom.Custom()

tic = time.perf_counter()
with open(r"C:\Users\fooba\repos\cpython\Objects\words.txt") as f:
    i = 0
    for word in f:
        mycustom.update({word: i})
        i += 1
toc = time.perf_counter()
print(str(toc - tic) + " seconds")

for key in list(mycustom.keys()):
    # print(mycustom.get(key))
    None