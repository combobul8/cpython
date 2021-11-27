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
        # print("\n" + word, flush = True)
        # print("dictionary size: " + str(len(mycustom.keys())), flush = True)

        if i >= 2:
            break
            '''elif mycustom.get(word) != None:
            # print("already seen " + word, flush = True)
            i += 1
            None'''
        else:
            mycustom.update({word: i})
            # words.update({word: i})

            i += 1

        # print("i: " + str(i))
        # print("")

toc = time.perf_counter()
# print(str((toc - tic) * 1000) + " milliseconds")

print("# keys: " + str(len(mycustom.keys())), flush = True)

# j = 0
for key in list(mycustom.keys()):
    # if j == 100:
        # print("j: " + str(j))
    # j += 1

    print(mycustom.get(key))
    # mycustom.get(key)
    None