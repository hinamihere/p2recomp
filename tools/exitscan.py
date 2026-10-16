import os,struct,subprocess,sys,concurrent.futures as cf
ROOT=os.getcwd()
EXE=os.path.join(ROOT,"build","dev","p2.exe")
slot=sys.argv[1]; mapf=sys.argv[2]; lo=int(sys.argv[3]); hi=int(sys.argv[4])
v0=struct.unpack("<IIIQ",open(f"states/slot{slot}.p2s","rb").read(20))[3]
base=[l for l in open(mapf) if not l.startswith(('EV ','EXIT '))]
cells=sorted({(int(l.split()[1]),int(l.split()[2])) for l in open(mapf) if l.startswith('EV ') and l.split()[3]=='4'})[lo:hi]
os.makedirs('build/exitscan',exist_ok=True)
def run(c):
    x,z=c
    mp=f'build/exitscan/m_{x}_{z}.txt'
    open(mp,'w').write(''.join(base)+f'EXIT {x} {z} 4\n')
    env=dict(os.environ,P2_NO_ENCOUNTERS="1",P2_NO_BATTLE_DETECT="1",P2_NO_DIALOGUE_DETECT="1",P2_TOUR=f"{mp},0,4",P2_TOUR_SPREAD="0")
    r=subprocess.run([EXE,"--headless","--turbo-until","99999999","--load-state",slot,"--exit-after",str(v0+9000)],env=env,cwd=ROOT,capture_output=True,text=True,errors="replace")
    t=r.stdout+r.stderr
    ac=[l[:150] for l in t.splitlines() if 'AREA CHANGE' in l]
    nr='no known route' in t
    return c,ac,nr
with cf.ThreadPoolExecutor(4) as ex:
    for c,ac,nr in ex.map(run,cells):
        print(c,'NOROUTE' if nr else '',ac[:1],flush=True)
