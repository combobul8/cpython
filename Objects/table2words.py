fout = open(r"C:\Users\fooba\repos\cpython\Objects\words.txt", "w")

with open(r"C:\Users\fooba\repos\cpython\Objects\iweb_wordFreq_sample_forms.txt") as fin:
    for i in range(24):
        fin.readline()

    while (toks := fin.readline().split()):
        fout.write(toks[3] + '\n')

fout.close()