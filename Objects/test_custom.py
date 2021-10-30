import custom
import time

mycustom = custom.Custom()
words = {}

tic = time.perf_counter()
with open(r"C:\Users\fooba\repos\cpython\Objects\words.txt") as f:
    i = 0
    for line in f:
        word = line.strip()
        # print(word)

        if i >= 5000:
            break
        elif word in words:
            print("already seen " + word)
        else:
            mycustom.update({word: i})
            words.update({word: i})

        i += 1
        # print("i: " + str(i))

toc = time.perf_counter()
# print(str((toc - tic) * 1000) + " milliseconds")

# print(len(mycustom.keys()))
for key in list(mycustom.keys()):
    # print(mycustom.get(key))
    mycustom.get(key)
    None