import custom
import time

mycustom = custom.Custom()

tic = time.perf_counter()
with open(r"C:\Users\fooba\repos\cpython\Objects\words.txt") as f:
    i = 0
    for line in f:
        word = line.strip()
        # print(word)

        if i < 1024:
            mycustom.update({word: i})

        i += 1

        if i >= 1024:
            # break
            mycustom.get(word)
        if i >= 2048:
            break
toc = time.perf_counter()
# print(str((toc - tic) * 1000) + " milliseconds")

for key in list(mycustom.keys()):
    # print(mycustom.get(key))
    # mycustom.get(key)
    None