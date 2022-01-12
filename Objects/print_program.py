import sys

print(sys.argv[1])
print(sys.argv[2])

with open(sys.argv[1], 'w') as fout:
    fout.write("import custom2\n")
    fout.write("import time\n")
    fout.write("\n")
    fout.write("mycustom = custom2.Custom()\n")

    words = {}
    with open(r"C:\Users\fooba\repos\cpython\Objects\words.txt") as fin:
        i = 0
        for line in fin:
            word = line.strip()

            if i >= int(sys.argv[2]):
                break

            fout.write("mycustom.update({\"" + word + "\": " + str(i) + "})\n")

            if words.get(word) == None:
                i += 1
                words.update({word: i})
            # else:
                # print("words.get(" + word + ") returned: " + str(words.get(word)))

    fout.write("mycustom.print()\n")