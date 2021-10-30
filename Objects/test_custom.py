import custom
import time

mycustom = custom.Custom()
words = {}

tic = time.perf_counter()
with open(r"C:\Users\fooba\repos\cpython\Objects\test.txt") as f:
    i = 0
    for line in f:
        word = line.strip()
        # print(word)

        if i >= 5000:
            break
        elif mycustom.get(word) != None:
            print("already seen " + word)
            i += 1
        else:
            mycustom.update({word: i})
            # words.update({word: i})
            i += 1

        # print("i: " + str(i))
        # print("")

toc = time.perf_counter()
# print(str((toc - tic) * 1000) + " milliseconds")

# print(len(mycustom.keys()))
for key in list(mycustom.keys()):
    print(mycustom.get(key))
    # mycustom.get(key)
    None