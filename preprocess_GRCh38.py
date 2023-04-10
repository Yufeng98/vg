import sys

filename = sys.argv[1]

ref_lst = ["chr" + str(i) for i in range(1, 23)] + ["chrX", "chrY"]

f = open(filename, "r")
lines = f.readlines()
f_o = open("tmp", "w")
flag = False
for line in lines:
    if line[0] == ">":
        lst = line.split()
        if lst[0][1:] in ref_lst:
            flag = True
            f_o = open(lst[0][1:]+".fa", "w")
    if flag:
        if line[0] == ">":
            f_o.write(">" + line[4:])
        else:
            f_o.write(line)
            