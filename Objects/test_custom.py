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
            print("")
            break
        elif mycustom.get(word) != None:
            # print("already seen " + word)
            # i += 1
            None
        else:
            mycustom.update({word: i})
            # words.update({word: i})
            i += 1

        # print("i: " + str(i))
        # print("")

toc = time.perf_counter()
# print(str((toc - tic) * 1000) + " milliseconds")

# print(len(mycustom.keys()))

# j = 0
for key in list(mycustom.keys()):
    # if j == 100:
        # print("j: " + str(j))
    # j += 1

    # print(mycustom.get(key))
    mycustom.get(key)
    None