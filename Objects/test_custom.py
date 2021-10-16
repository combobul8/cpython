import custom

mycustom = custom.Custom()

with open(r"C:\Users\fooba\repos\cpython\Objects\iweb_wordFreq_sample_forms.txt") as f:
    for i in range(24):
        f.readline()

    i = 0
    while (toks := f.readline().split()):
        # print(toks[3])
        mycustom.update({toks[3]: i})
        i += 1

        if i >= 10:
            break

for key in list(mycustom.keys()):
    print(mycustom.get(key))