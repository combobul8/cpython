print("import custom2")
print("import time")
print()
print("mycustom = custom2.Custom()")

words = {}
with open(r"C:\Users\fooba\repos\cpython\Objects\words.txt") as f:
    i = 0
    for line in f:
        word = line.strip()

        if i >= 2:
            break

        print("mycustom.update({\"" + word + "\": " + str(i) + "})")

        if words.get(word) == None:
            i += 1