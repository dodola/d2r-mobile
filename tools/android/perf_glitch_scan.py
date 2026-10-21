import sys,cv2,numpy as np
K=6
for path in sys.argv[1:]:
    cap=cv2.VideoCapture(path);fr=[]
    while True:
        ok,f=cap.read()
        if not ok:break
        fr.append(cv2.cvtColor(cv2.resize(f,(280,126)),cv2.COLOR_BGR2GRAY).astype(np.float32))
    n=len(fr);D=lambda a,b:float(np.abs(fr[a]-fr[b]).mean())
    bad=[i for i in range(K,n-K) if min(D(i,i-K),D(i,i+K))>20 and D(i-K,i+K)<6]
    bursts=[]
    for i in bad:
        if bursts and i-bursts[-1][1]<=2:bursts[-1][1]=i
        else:bursts.append([i,i])
    print(path,'frames',n,'bad',len(bad),'bursts',[(round(a/60,1),b-a+1) for a,b in bursts][:30])
