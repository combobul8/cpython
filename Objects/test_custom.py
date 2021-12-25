import custom2
import time

mycustom = custom2.Custom()
words = {}

tic = time.perf_counter()
with open(r"C:\Users\fooba\repos\cpython\Objects\words.txt") as f:
    i = 0
    flag = False

    for line in f:
        word = line.strip()

        if i >= 1:
            break
        elif mycustom.get(word) == None:
            mycustom.update({word: i})
            words[word] = i
            i += 1
        else:
            print("already seen " + word, flush = True)
            None

toc = time.perf_counter()
# print(str((toc - tic) * 1000) + " milliseconds")

print("# keys: " + str(len(mycustom.keys())), flush = True)

for key in list(words.keys()):
    # print(key)
    if mycustom.get(key):
        print(key)
    break
    None