// (c) 2024, RetiredCoder
// License: GPLv3, see "LICENSE.TXT" file
// https://github.com/RetiredC/Kang-1

#pragma warning(disable : 4996)

#include "defs.h"

#include "framework.h"
#include "ExpKang.h"
#include "ExpKangDlg.h"
#include "afxdialogex.h"
#include <vector>

#include "Ec.h"
#include "utils.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


#define RANGE_BITS			(32 + 8)
#define DP_BITS				(5)
#define KANG_CNT			512
//#define KANG_CNT			(1 << (int)(0.25 * RANGE_BITS))
#define CPU_THR_CNT			(63)
#define POINTS_CNT			(1000)

#define JMP_CNT				(16 * 1024)
#define OLD_LEN				(16)

//#define INTERVAL_STATS

#ifdef INTERVAL_STATS
	#define INTERVAL_BITS	5
	#define INTERVAL_CNT	(1 << INTERVAL_BITS)
	int int_cnt[INTERVAL_CNT];
	u64 int_sum[INTERVAL_CNT];
#endif

void ToLog(char* str);

CExpKangDlg* dlg;

CExpKangDlg::CExpKangDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_EXPKANG_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CExpKangDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_LS_LOG, lsLog);
	DDX_Control(pDX, IDC_STATIC_TM, lbTime);
}

BEGIN_MESSAGE_MAP(CExpKangDlg, CDialogEx)
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
    ON_BN_CLICKED(IDC_BUTTON1, &CExpKangDlg::OnBnClickedButton1)
	ON_BN_CLICKED(IDC_BUTTON2, &CExpKangDlg::OnBnClickedButton2)
	ON_BN_CLICKED(IDC_BUTTON3, &CExpKangDlg::OnBnClickedButton3)
	ON_BN_CLICKED(IDC_BUTTON4, &CExpKangDlg::OnBnClickedButton4)
	ON_BN_CLICKED(IDC_BUTTON5, &CExpKangDlg::OnBnClickedButton5)
END_MESSAGE_MAP()

BOOL CExpKangDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();
	SetIcon(m_hIcon, TRUE);
	SetIcon(m_hIcon, FALSE);
	dlg = this;
	InitEc();
	char s[100];
	sprintf(s, "Range: %d bits. DP: %d bits. Kangaroos: %d. Threads: %d. Points in test: %d", RANGE_BITS, DP_BITS, KANG_CNT, CPU_THR_CNT, POINTS_CNT);
	ToLog(s);
	return TRUE;  // return TRUE  unless you set the focus to a control
}

void CExpKangDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // device context for painting
		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
		CDialogEx::OnPaint();
}

// The system calls this function to obtain the cursor to display while the user drags
//  the minimized window.
HCURSOR CExpKangDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

bool DoEvents()
{
	MSG msg;
	while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
		{
			return false;
		}
		if (!AfxGetApp()->PreTranslateMessage(&msg))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
	}
	return true;
}

void ToLog(char* str)
{
	dlg->lsLog.AddString(str);
	DoEvents();
}

struct EcJMP
{
	EcPoint p;
	EcInt dist;
};
EcJMP EcJumps[JMP_CNT];

struct EcKang
{
	EcPoint p;
	EcInt dist;
	int iter; //iters without new DP
};
typedef std::vector <EcKang> EcKangs;

EcPoint Pnt_HalfRange;
EcPoint Pnt_NegHalfRange;
EcInt Int_HalfRange;
EcInt Int_TameOffset; //for 3-way only
EcPoint Pnt_TameOffset;

EcInt x32;
EcPoint Pntx32;

Ec ec;

volatile long ThrCnt;
volatile long SolvedCnt;
volatile long ToSolveCnt;

struct TThrRec
{
	HANDLE hThread;
	CExpKangDlg* obj;
	size_t iters;
	int thr_ind;
};


#define TAME	0
#define WILD	1
#define WILD2	2

struct TDB_Rec
{
	BYTE x[12];
	BYTE d[12];
	int type; //0 - tame, 1 - wild1, 2 - wild2
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Paper: a lot, just google :)

bool Collision_Classic(EcPoint& pnt, EcInt t, EcInt w)
{
	t.Sub(w);
	t.Add(Int_HalfRange);
	EcPoint P = ec.MultiplyG(t);
	return P.IsEqual(pnt);
}

u32 __stdcall thr_proc_classic(void* data)
{
	TThrRec* rec = (TThrRec*)data;
	rec->iters = 0;
	EcKangs kangs;
	kangs.resize(KANG_CNT);
	u32 DPmask = (1 << DP_BITS) - 1;
	TFastBase* db = new TFastBase();
	db->Init(sizeof(TDB_Rec::x), sizeof(TDB_Rec), 0, 0);

	while (1)
	{
		if (InterlockedDecrement(&ToSolveCnt) < 0)
			break;

		EcInt KToSolve;
		EcPoint PointToSolve;
		u64 pnt_iters = 0;
		KToSolve.RndBits(RANGE_BITS);

		for (int i = 0; i < KANG_CNT; i++)
			kangs[i].dist.RndBits(RANGE_BITS);
		PointToSolve = ec.MultiplyG(KToSolve);
		for (int i = 0; i < KANG_CNT; i++)
			kangs[i].p = ec.MultiplyG(kangs[i].dist);

		EcPoint Pnt = ec.AddPoints(PointToSolve, Pnt_NegHalfRange);
		for (int i = KANG_CNT / 2; i < KANG_CNT; i++)
			kangs[i].p = ec.AddPoints(kangs[i].p, Pnt);

		bool found = false;
		while (!found)
		{
			for (int i = 0; i < KANG_CNT; i++)
			{
				int jmp_ind = kangs[i].p.x.data[0] & (JMP_CNT - 1);
				kangs[i].p = ec.AddPoints(kangs[i].p, EcJumps[jmp_ind].p);
				kangs[i].dist.Add(EcJumps[jmp_ind].dist);	
				rec->iters++;
				pnt_iters++;

				if (kangs[i].p.x.data[0] & DPmask)
					continue;

				TDB_Rec nrec;
				memcpy(nrec.x, kangs[i].p.x.data, 12);
				memcpy(nrec.d, kangs[i].dist.data, 12);
				nrec.type = (i < KANG_CNT / 2) ? TAME : WILD;
				TDB_Rec* pref = (TDB_Rec*)db->FindOrAddDataBlock((BYTE*)&nrec, sizeof(nrec));
				if (pref)
				{
					if (pref->type == nrec.type)
						continue;

					EcInt w, t;
					if (pref->type == WILD)
					{
						memcpy(w.data, pref->d, sizeof(pref->d));
						memcpy(t.data, nrec.d, sizeof(nrec.d));
					}
					else
					{
						memcpy(w.data, nrec.d, sizeof(nrec.d));
						memcpy(t.data, pref->d, sizeof(pref->d));
					}
					bool res = Collision_Classic(PointToSolve, t, w);
					if (!res) //ignore mirrored collisions
						continue;
					found = true;
					break;
				}
			}
		}
#ifdef INTERVAL_STATS
		KToSolve.ShiftRight(RANGE_BITS - INTERVAL_BITS);
		int int_ind = (int)KToSolve.data[0];
		int_cnt[int_ind]++;
		int_sum[int_ind] += pnt_iters;
#endif
		db->Clear(false);
		InterlockedIncrement(&SolvedCnt);
	}
	delete db;
	InterlockedDecrement(&ThrCnt);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Paper: a lot, just google :)
// they promise K=1.7, but there is a trick to improve it to K=1.6

bool Collision_3way(EcPoint& pnt, EcInt t, int TameType, EcInt w, int WildType)
{
	if (TameType == TAME)
	{
		t.Add(Int_TameOffset);
		EcInt pk = t;
		pk.Sub(w);
		EcInt sv = pk;
		pk.Add(Int_HalfRange);
		EcPoint P = ec.MultiplyG(pk);
		if (P.IsEqual(pnt))
			return true;
		pk = sv;
		pk.Neg();
		pk.Add(Int_HalfRange);
		P = ec.MultiplyG(pk);
		return P.IsEqual(pnt);
	}
	else //two wild
	{
		EcInt pk = t;
		pk.Sub(w);
		if (pk.data[4] >> 63)
			pk.Neg();
		pk.ShiftRight(1);
		EcInt sv = pk;
		pk.Add(Int_HalfRange);
		EcPoint P = ec.MultiplyG(pk);
		if (P.IsEqual(pnt))
			return true;
		pk = sv;
		pk.Neg();
		pk.Add(Int_HalfRange);
		P = ec.MultiplyG(pk);
		return P.IsEqual(pnt);
	}
}

u32 __stdcall thr_proc_3way(void* data)
{
	TThrRec* rec = (TThrRec*)data;
	rec->iters = 0;
	EcKangs kangs;
	kangs.resize(KANG_CNT);
	u32 DPmask = (1 << DP_BITS) - 1;
	TFastBase* db = new TFastBase();
	db->Init(sizeof(TDB_Rec::x), sizeof(TDB_Rec), 0, 0);
	while (1)
	{
		if (InterlockedDecrement(&ToSolveCnt) < 0)
			break;
		EcInt KToSolve;
		EcPoint PointToSolve;
		EcPoint NegPointToSolve;
		u64 pnt_iters = 0;
		KToSolve.RndBits(RANGE_BITS);

		for (int i = 0; i < KANG_CNT; i++)
		{
			if (i < KANG_CNT / 3)
				kangs[i].dist.RndBits(RANGE_BITS - 4);
			else
				kangs[i].dist.RndBits(RANGE_BITS);

			if (i >= KANG_CNT / 3)
				kangs[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
		}

		PointToSolve = ec.MultiplyG(KToSolve);
		EcPoint Pnt1 = ec.AddPoints(PointToSolve, Pnt_NegHalfRange);
		EcPoint Pnt2 = Pnt1;
		Pnt2.y.NegModP();

		for (int i = 0; i < KANG_CNT; i++)
			kangs[i].p = ec.MultiplyG(kangs[i].dist);	
		for (int i = 0; i < KANG_CNT / 3; i++) //tame
			kangs[i].p = ec.AddPoints(kangs[i].p, Pnt_TameOffset);	
		for (int i = KANG_CNT / 3; i < 2 * KANG_CNT / 3; i++) //wild1
			kangs[i].p = ec.AddPoints(kangs[i].p, Pnt1);	
		for (int i = 2 * KANG_CNT / 3; i < KANG_CNT; i++) //wild2
			kangs[i].p = ec.AddPoints(kangs[i].p, Pnt2);

		bool found = false;
		while (!found)
		{
			for (int i = 0; i < KANG_CNT; i++)
			{
				int jmp_ind = kangs[i].p.x.data[0] & (JMP_CNT - 1);
				kangs[i].p = ec.AddPoints(kangs[i].p, EcJumps[jmp_ind].p);
				kangs[i].dist.Add(EcJumps[jmp_ind].dist);
				rec->iters++;
				pnt_iters++;

				if (kangs[i].p.x.data[0] & DPmask)
					continue;

				TDB_Rec nrec;
				memcpy(nrec.x, kangs[i].p.x.data, 12);
				memcpy(nrec.d, kangs[i].dist.data, 12);
				if (i < KANG_CNT / 3)
					nrec.type = TAME;
				else
					if (i < 2 * KANG_CNT / 3)
						nrec.type = WILD;
					else
						nrec.type = WILD2;

				bool same = false;
				TDB_Rec* pref = (TDB_Rec*)db->FindOrAddDataBlock((BYTE*)&nrec, sizeof(nrec));
				if (pref)
				{
					if (pref->type == nrec.type)
						continue; //we ignore mirror collisions because high DP will eliminate them

					EcInt w, t;
					int TameType, WildType;
					if (pref->type != TAME)
					{
						memcpy(w.data, pref->d, sizeof(pref->d));
						memcpy(t.data, nrec.d, sizeof(nrec.d));
						TameType = nrec.type;
						WildType = pref->type;
					}
					else
					{
						memcpy(w.data, nrec.d, sizeof(nrec.d));
						memcpy(t.data, pref->d, sizeof(pref->d));
						TameType = TAME;
						WildType = nrec.type;
					}

					bool res = Collision_3way(PointToSolve, t, TameType, w, WildType);
					if (!res)
						continue;
					found = true;
					break;
				}
			}
		}
#ifdef INTERVAL_STATS
		KToSolve.ShiftRight(RANGE_BITS - INTERVAL_BITS);
		int int_ind = (int)KToSolve.data[0];
		int_cnt[int_ind]++;
		int_sum[int_ind] += pnt_iters;
#endif
		db->Clear(false);
		InterlockedIncrement(&SolvedCnt);
	}
	delete db;
	InterlockedDecrement(&ThrCnt);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Paper: "Using Equivalence Classes to Accelerate Solving the Discrete Logarithm Problem in a Short Interval", 2010
// Paper: "A variant of the Galbraith–Ruprai algorithm for discrete logarithms with improved complexity", 2018

bool Collision_Mirror(EcPoint& pnt, EcInt t, EcInt w, bool same)
{
	if (same)
	{
		t.Neg();		
		EcInt pk = t;
		pk.Sub(w);
		if (pk.data[4] >> 63)
			pk.Neg();
		pk.ShiftRight(1);
		EcInt sv = pk;
		pk.Add(Int_HalfRange);
		EcPoint P = ec.MultiplyG(pk);
		if (P.IsEqual(pnt))
			return true;
		pk = sv;
		pk.Neg();
		pk.Add(Int_HalfRange);
		P = ec.MultiplyG(pk);
		return P.IsEqual(pnt);		
	}
	else
	{
		EcInt pk = t;
		pk.Sub(w);
		pk.Add(Int_HalfRange);
		EcPoint P = ec.MultiplyG(pk);
		if (P.IsEqual(pnt))
			return true;
		t.Neg();
		pk = t;
		pk.Sub(w);
		pk.Add(Int_HalfRange);
		P = ec.MultiplyG(pk);
		return P.IsEqual(pnt);
	}
}

u32 __stdcall thr_proc_mirror(void* data)
{
	TThrRec* rec = (TThrRec*)data;
	rec->iters = 0;
	EcKangs kangs;
	kangs.resize(KANG_CNT);
	u32 DPmask = (1 << DP_BITS) - 1;
	TFastBase* db = new TFastBase();
	db->Init(sizeof(TDB_Rec::x), sizeof(TDB_Rec), 0, 0);
	u64* old = (u64*)malloc(OLD_LEN * 8 * KANG_CNT);
	int max_iters = (1 << DP_BITS) * 20;

	EcInt tames_range;
	tames_range.Set(1);
	tames_range.ShiftLeft(RANGE_BITS - 1);
	EcInt wild_range;
	wild_range.Set(1);
	wild_range.ShiftLeft(RANGE_BITS - 4);

	//make K better for points near edges of the range, define INTERVAL_STATS to see the difference
	EcInt small_ext = wild_range;
	tames_range.Add(small_ext);

	while (1)
	{
		if (InterlockedDecrement(&ToSolveCnt) < 0)
			break;
		EcInt KToSolve;
		EcPoint PointToSolve;
		EcPoint NegPointToSolve;
		u64 pnt_iters = 0;
		memset(old, 0, OLD_LEN * 8 * KANG_CNT);
		KToSolve.RndBits(RANGE_BITS);

		for (int i = 0; i < KANG_CNT / 2; i++)
			kangs[i].dist.RndMax(tames_range); //tame
		for (int i = KANG_CNT / 2; i < KANG_CNT; i++)
				kangs[i].dist.RndMax(wild_range);

		PointToSolve = ec.MultiplyG(KToSolve);
		EcPoint Pnt = ec.AddPoints(PointToSolve, Pnt_NegHalfRange);
		for (int i = 0; i < KANG_CNT; i++)
		{
			kangs[i].p = ec.MultiplyG(kangs[i].dist);
			kangs[i].iter = 0;
		}
		for (int i = KANG_CNT / 2; i < KANG_CNT; i++)
			kangs[i].p = ec.AddPoints(kangs[i].p, Pnt);

		bool found = false;
		while (!found)
		{
			for (int i = 0; i < KANG_CNT; i++)
			{
				bool inv = (kangs[i].p.y.data[0] & 1);				
				bool cycled = false;
				for (int j = 0; j < OLD_LEN; j++)
					if (old[OLD_LEN * i + j] == kangs[i].dist.data[0])
					{
						cycled = true;
						break;
					}
				old[OLD_LEN * i + (kangs[i].iter % OLD_LEN)] = kangs[i].dist.data[0];
				kangs[i].iter++;
				if (kangs[i].iter > max_iters)
					cycled = true;
				if (cycled)
				{
					if (i < KANG_CNT / 2)
						kangs[i].dist.RndMax(tames_range); //tame
					else
						kangs[i].dist.RndMax(wild_range);
					kangs[i].p = ec.MultiplyG(kangs[i].dist);
					if (i >= KANG_CNT / 2)
						kangs[i].p = ec.AddPoints(kangs[i].p, Pnt);
					kangs[i].iter = 0;
					memset(&old[OLD_LEN * i], 0, 8 * OLD_LEN);
					continue;
				}

				int jmp_ind = kangs[i].p.x.data[0] % JMP_CNT;
				EcPoint AddP = EcJumps[jmp_ind].p;
				if (!inv)
				{
					kangs[i].p = ec.AddPoints(kangs[i].p, AddP);
					kangs[i].dist.Add(EcJumps[jmp_ind].dist);
				}
				else
				{
					AddP.y.NegModP();
					kangs[i].p = ec.AddPoints(kangs[i].p, AddP);
					kangs[i].dist.Sub(EcJumps[jmp_ind].dist);
				}
				rec->iters++;
				pnt_iters++;

				if (kangs[i].p.x.data[0] & DPmask)
					continue;

				TDB_Rec nrec;
				memcpy(nrec.x, kangs[i].p.x.data, 12);
				memcpy(nrec.d, kangs[i].dist.data, 12);
				if (i < KANG_CNT / 2)
					nrec.type = TAME;
				else
					nrec.type = WILD;

				bool same = false;
				TDB_Rec* pref = (TDB_Rec*)db->FindOrAddDataBlock((BYTE*)&nrec, sizeof(nrec));
				if (pref)
				{
					if (pref->type == nrec.type)
					{
						if (pref->type == TAME)
							continue;

						//if it's wild, we can find the key from the same type if distances are different
						if (*(u64*)pref->d == *(u64*)nrec.d)
							continue;
						else
						{							
							same = true;
							//ToLog("key found by same wild");
						}
					}

					EcInt w, t;
					if (pref->type != TAME)
					{
						memcpy(w.data, pref->d, sizeof(pref->d)); 
						if (pref->d[11] == 0xFF) memset(((BYTE*)w.data) + 12, 0xFF, 28);
						memcpy(t.data, nrec.d, sizeof(nrec.d)); 
						if (nrec.d[11] == 0xFF) memset(((BYTE*)t.data) + 12, 0xFF, 28);
					}
					else
					{
						memcpy(w.data, nrec.d, sizeof(nrec.d)); 
						if (nrec.d[11] == 0xFF) memset(((BYTE*)w.data) + 12, 0xFF, 28);
						memcpy(t.data, pref->d, sizeof(pref->d)); 
						if (pref->d[11] == 0xFF) memset(((BYTE*)t.data) + 12, 0xFF, 28);
					}

					bool res = Collision_Mirror(PointToSolve, t, w, same);
					if (!res)
						continue;

					found = true;
					break;
				}
				else
				{
					kangs[i].iter = 0;
					memset(&old[OLD_LEN * i], 0, 8 * OLD_LEN);
				}
			}
		}
#ifdef INTERVAL_STATS
		KToSolve.ShiftRight(RANGE_BITS - INTERVAL_BITS);
		int int_ind = (int)KToSolve.data[0];
		int_cnt[int_ind]++;
		int_sum[int_ind] += pnt_iters;
#endif
		db->Clear(false);
		InterlockedIncrement(&SolvedCnt);
	}
	////
	free(old);
	delete db;
	InterlockedDecrement(&ThrCnt);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// I couldn’t find any papers about this method, so let's assume that I invented it. :)
// it gives best K=1.15

EcInt WildRange;
EcInt TameRange;


bool Collision_SOTA(EcPoint& pnt, EcInt t, int TameType, EcInt w, int WildType, bool IsNeg)
{
	if (IsNeg)
		t.Neg();
	if (TameType == TAME)
	{
		EcInt pk = t;
		pk.Sub(w);
		EcInt sv = pk;
		EcPoint P = ec.MultiplyG(pk);
		if (P.IsEqual(pnt))
			return true;
		pk = sv;
		pk.Neg();
		P = ec.MultiplyG(pk);
		if (P.IsEqual(pnt))
			return true;
		return false;
	}
	else
	{
		EcInt pk = t;
		pk.Sub(w);
		if (pk.data[4] >> 63)
			pk.Neg();
		pk.ShiftRight(1);
		EcInt sv = pk;
		EcPoint P = ec.MultiplyG(pk);
		if (P.IsEqual(pnt))
			return true;
		pk = sv;
		pk.Neg();
		P = ec.MultiplyG(pk);
		return P.IsEqual(pnt);
	}
}

u32 __stdcall thr_proc_sota(void* data)
{
	TThrRec* rec = (TThrRec*)data;
	rec->iters = 0;
	EcKangs kangs;
	kangs.resize(KANG_CNT);
	u32 DPmask = (1 << DP_BITS) - 1;
	TFastBase* db = new TFastBase();
	db->Init(sizeof(TDB_Rec::x), sizeof(TDB_Rec), 0, 0);

	u64* old = (u64*)malloc(OLD_LEN * 8 * KANG_CNT);
	int max_iters = (1 << DP_BITS) * 20;

	while (1)
	{
		if (InterlockedDecrement(&ToSolveCnt) < 0)
			break;
		EcInt KToSolve;
		EcPoint PointToSolve;
		EcPoint NegPointToSolve;
		u64 pnt_iters = 0;
		memset(old, 0, OLD_LEN * 8 * KANG_CNT);
		KToSolve.RndBits(RANGE_BITS);

		for (int i = 0; i < KANG_CNT; i++)
		{
			if (i < KANG_CNT / 3)
				kangs[i].dist.RndMax(TameRange);
			else
			{
				kangs[i].dist.RndMax(WildRange);
				kangs[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
			}
		}

		KToSolve.Add(x32); //add x32 just to improve K for range edges
		PointToSolve = ec.MultiplyG(KToSolve);

		EcPoint PntWild = PointToSolve; // ec.AddPoints(PointToSolve, Pntx32); //just to improve K for range edges
		PntWild.y.NegModP(); //negate wild point to be able to place kangs with positive offsets

		for (int i = 0; i < KANG_CNT; i++)
		{
			kangs[i].p = ec.MultiplyG(kangs[i].dist);
			kangs[i].iter = 0;
		}

		for (int i = KANG_CNT / 3; i < KANG_CNT; i++)
			kangs[i].p = ec.AddPoints(kangs[i].p, PntWild);

		bool found = false;
		while (!found)
		{
			for (int i = 0; i < KANG_CNT; i++)
			{
				bool inv = (kangs[i].p.y.data[0] & 1);
				bool cycled = false;
				for (int j = 0; j < OLD_LEN; j++)
					if (old[OLD_LEN * i + j] == kangs[i].dist.data[0])
					{
						cycled = true;
						break;
					}
				old[OLD_LEN * i + (kangs[i].iter % OLD_LEN)] = kangs[i].dist.data[0];
				kangs[i].iter++;
				if (kangs[i].iter > max_iters)
					cycled = true;
				if (cycled)
				{
					if (i < KANG_CNT / 3)
						kangs[i].dist.RndMax(TameRange);
					else
					{
						kangs[i].dist.RndMax(WildRange);
						kangs[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
					}

					kangs[i].iter = 0;
					kangs[i].p = ec.MultiplyG(kangs[i].dist);
					if (i >= KANG_CNT / 3)
						kangs[i].p = ec.AddPoints(kangs[i].p, PntWild);

					memset(&old[OLD_LEN * i], 0, 8 * OLD_LEN);
					continue;
				}

				int jmp_ind = kangs[i].p.x.data[0] % JMP_CNT;
				EcPoint AddP = EcJumps[jmp_ind].p;
				if (!inv)
				{
					kangs[i].p = ec.AddPoints(kangs[i].p, AddP);
					kangs[i].dist.Add(EcJumps[jmp_ind].dist);
				}
				else
				{
					AddP.y.NegModP();
					kangs[i].p = ec.AddPoints(kangs[i].p, AddP);
					kangs[i].dist.Sub(EcJumps[jmp_ind].dist);
				}
				rec->iters++;
				pnt_iters++;

				if (kangs[i].p.x.data[0] & DPmask)
					continue;

				TDB_Rec nrec;
				memcpy(nrec.x, kangs[i].p.x.data, 12);
				memcpy(nrec.d, kangs[i].dist.data, 12); 
				if (i < KANG_CNT / 3)
					nrec.type = TAME;
				else
					nrec.type = WILD;

				TDB_Rec* pref = (TDB_Rec*)db->FindOrAddDataBlock((BYTE*)&nrec, sizeof(nrec));
				if (pref)
				{
					if (pref->type == nrec.type)
					{
						if (pref->type == TAME)
							continue;

						//if it's wild, we can find the key from the same type if distances are different
						if (*(u64*)pref->d == *(u64*)nrec.d)
							continue;
						//else
						//	ToLog("key found by same wild");
					}

					EcInt w, t;
					int TameType, WildType;
					if (pref->type != TAME)
					{
						memcpy(w.data, pref->d, sizeof(pref->d)); 
						if (pref->d[11] == 0xFF) memset(((BYTE*)w.data) + 12, 0xFF, 28);
						memcpy(t.data, nrec.d, sizeof(nrec.d)); 
						if (nrec.d[11] == 0xFF) memset(((BYTE*)t.data) + 12, 0xFF, 28);
						TameType = nrec.type;
						WildType = pref->type;
					}
					else
					{
						memcpy(w.data, nrec.d, sizeof(nrec.d)); 
						if (nrec.d[11] == 0xFF) memset(((BYTE*)w.data) + 12, 0xFF, 28);
						memcpy(t.data, pref->d, sizeof(pref->d)); 
						if (pref->d[11] == 0xFF) memset(((BYTE*)t.data) + 12, 0xFF, 28);
						TameType = TAME;
						WildType = nrec.type;
					}

					bool res = Collision_SOTA(PointToSolve, t, TameType, w, WildType, false) || Collision_SOTA(PointToSolve, t, TameType, w, WildType, true);
					if (!res)
					{
						res = Collision_SOTA(PointToSolve, t, TameType, w, WildType, false) || Collision_SOTA(PointToSolve, t, TameType, w, WildType, true);
						//bool w12 = ((pref->type == WILD) && (nrec.type == WILD2)) || ((pref->type == WILD2) && (nrec.type == WILD));
						//if (w12) //in rare cases WILD and WILD2 can collide in mirror, in this case there is no way to find K
						//	ToLog("W1 and W2 collided in mirror");
						continue;
					}
					found = true;
					break;
				}
				else
				{
					kangs[i].iter = 0;
					memset(&old[OLD_LEN * i], 0, 8 * OLD_LEN);
				}
			}
		}
#ifdef INTERVAL_STATS
		KToSolve.Sub(x32);
		KToSolve.ShiftRight(RANGE_BITS - INTERVAL_BITS);
		int int_ind = (int)KToSolve.data[0];
		int_cnt[int_ind]++;
		int_sum[int_ind] += pnt_iters;
#endif
		db->Clear(false);
		InterlockedIncrement(&SolvedCnt);
	}
	free(old);
	delete db;
	InterlockedDecrement(&ThrCnt);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//This method is the same as SOTA, but also uses cheap second point.
//When we calculate "NextPoint = PreviousPoint + JumpPoint" we can also quickly calculate "PreviousPoint - JumpPoint" because inversion is the same.
//If inversion calculation takes a lot of time, this second point is cheap for us and we can use it to improve K.
//Using cheap point costs only(1MUL + 1SQR) / 2. K is approximately 1.02 for this method(assuming cheap point is free and not counted as 1op).

u32 __stdcall thr_proc_sota_plus(void* data)
{
	TThrRec* rec = (TThrRec*)data;
	rec->iters = 0;
	EcKangs kangs;
	kangs.resize(KANG_CNT);
	u32 DPmask = (1 << DP_BITS) - 1;
	TFastBase* db = new TFastBase();
	db->Init(sizeof(TDB_Rec::x), sizeof(TDB_Rec), 0, 0);

	u64* old = (u64*)malloc(OLD_LEN * 8 * KANG_CNT);
	int max_iters = (1 << DP_BITS) * 20;

	while (1) 
	{
		if (InterlockedDecrement(&ToSolveCnt) < 0)
			break;
		EcInt KToSolve;
		EcPoint PointToSolve;
		EcPoint NegPointToSolve;
		u64 pnt_iters = 0;
		memset(old, 0, OLD_LEN * 8 * KANG_CNT);
		KToSolve.RndBits(RANGE_BITS);

		for (int i = 0; i < KANG_CNT; i++)
		{
			if (i < KANG_CNT / 3)
			{
				kangs[i].dist.RndMax(TameRange);
			}
			else
			{
				kangs[i].dist.RndMax(WildRange);
				kangs[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
			}
		}

		KToSolve.Add(x32); //add x32 just to improve K for range edges
		PointToSolve = ec.MultiplyG(KToSolve);

		EcPoint PntWild = PointToSolve; // ec.AddPoints(PointToSolve, Pntx32); //just to improve K for range edges
		PntWild.y.NegModP(); //negate wild point to be able to place kangs with positive offsets

		for (int i = 0; i < KANG_CNT; i++)
		{
			kangs[i].p = ec.MultiplyG(kangs[i].dist);
			kangs[i].iter = 0;
		}

		for (int i = KANG_CNT / 3; i < KANG_CNT; i++)
			kangs[i].p = ec.AddPoints(kangs[i].p, PntWild);

		bool found = false;
		while (!found)
		{
			for (int i = 0; i < KANG_CNT; i++)
			{
				bool inv = (kangs[i].p.y.data[0] & 1);
				bool cycled = false;
				for (int j = 0; j < OLD_LEN; j++)
					if (old[OLD_LEN * i + j] == kangs[i].dist.data[0])
					{
						cycled = true;
						break;
					}
				old[OLD_LEN * i + (kangs[i].iter % OLD_LEN)] = kangs[i].dist.data[0];
				kangs[i].iter++;
				if (kangs[i].iter > max_iters)
					cycled = true;
				if (cycled)
				{
					if (i < KANG_CNT / 3)
						kangs[i].dist.RndMax(TameRange);
					else
					{
						kangs[i].dist.RndMax(WildRange);
						kangs[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
					}

					kangs[i].iter = 0;
					kangs[i].p = ec.MultiplyG(kangs[i].dist);
					if (i >= KANG_CNT / 3)
						kangs[i].p = ec.AddPoints(kangs[i].p, PntWild);

					memset(&old[OLD_LEN * i], 0, 8 * OLD_LEN);
					continue;
				}

				int jmp_ind = kangs[i].p.x.data[0] % JMP_CNT;
				EcPoint AddP = EcJumps[jmp_ind].p;
				EcPoint Saved = kangs[i].p;
				EcInt SavedD = kangs[i].dist;
				EcInt inversion;
				if (!inv)
				{
					kangs[i].p = ec.AddPointsAndGetInv(kangs[i].p, AddP, inversion);
					kangs[i].dist.Add(EcJumps[jmp_ind].dist);
				}
				else
				{
					AddP.y.NegModP();
					kangs[i].p = ec.AddPointsAndGetInv(kangs[i].p, AddP, inversion);
					kangs[i].dist.Sub(EcJumps[jmp_ind].dist);
				}
				rec->iters++;
				pnt_iters++;

//when we calculate "NextPoint = PreviousPoint + JumpPoint" we can also quickly calculate "PreviousPoint - JumpPoint" because inversion is the same
//if inversion calculation takes a lot of time, this second point is cheap for us and we can use it to improve K
//using cheap point costs only (1MUL+1SQR)/2, this code is not optimized and always calculates Y for both points which is not necessary
//K is 1.02
/*
				if ((kangs[i].p.x.data[0] & 1) != 0)
				{
					//	rec->iters++; assume that point (PreviousPoint - JumpPoint) is cheap so we don't count it as 1op
					AddP.y.NegModP();
					EcPoint p2 = ec.AddPointsHaveInv(Saved, AddP, inversion);
					if ((p2.x.data[0] & 1) == 0)
					{
						kangs[i].p = p2;
						kangs[i].dist = SavedD;
						if (!inv)
							kangs[i].dist.Sub(EcJumps[jmp_ind].dist);
						else
							kangs[i].dist.Add(EcJumps[jmp_ind].dist);
					}
				}
/**/

//"lite" variant, pay only (1MUL+1SQR)/4, this code is not optimized and always calculates Y for both points which is not necessary
//K is 1.05
/*
				if ((kangs[i].p.x.data[0] & 3) == 3)
				{
					//	rec->iters++; assume that point (PreviousPoint - JumpPoint) is cheap so we don't count it as 1op
					AddP.y.NegModP();
					EcPoint p2 = ec.AddPointsHaveInv(Saved, AddP, inversion);
					if ((p2.x.data[0] & 3) != 3)
					{
						kangs[i].p = p2;
						kangs[i].dist = SavedD;
						if (!inv)
							kangs[i].dist.Sub(EcJumps[jmp_ind].dist);
						else
							kangs[i].dist.Add(EcJumps[jmp_ind].dist);
					}
				}
/**/

/**/
//for GPU, we dont get any speedup from first IF, so better to always calc both points and choose the best one
//this code is not optimized and always calculates Y for both points which is not necessary
//K is 0.99
				AddP.y.NegModP();
				EcPoint p2 = ec.AddPointsHaveInv(Saved, AddP, inversion);
				if ((p2.x.data[0] & 3) < (kangs[i].p.x.data[0] & 3))
				{
					kangs[i].p = p2;
					kangs[i].dist = SavedD;
					if (!inv)
						kangs[i].dist.Sub(EcJumps[jmp_ind].dist);
					else
						kangs[i].dist.Add(EcJumps[jmp_ind].dist);
				}
/**/


////
				if (kangs[i].p.x.data[0] & DPmask)
					continue;

				TDB_Rec nrec;
				memcpy(nrec.x, kangs[i].p.x.data, 12);
				memcpy(nrec.d, kangs[i].dist.data, 12);
				if (i < KANG_CNT / 3)
					nrec.type = TAME;
				else
					nrec.type = WILD;

				TDB_Rec* pref = (TDB_Rec*)db->FindOrAddDataBlock((BYTE*)&nrec, sizeof(nrec));
				if (pref)
				{
					if (pref->type == nrec.type)
					{
						if (pref->type == TAME)
							continue;

						//if it's wild, we can find the key from the same type if distances are different
						if (*(u64*)pref->d == *(u64*)nrec.d)
							continue;
						//else
						//	ToLog("key found by same wild");
					}

					EcInt w, t;
					int TameType, WildType;
					if (pref->type != TAME)
					{
						memcpy(w.data, pref->d, sizeof(pref->d));
						if (pref->d[11] == 0xFF) memset(((BYTE*)w.data) + 12, 0xFF, 28);
						memcpy(t.data, nrec.d, sizeof(nrec.d));
						if (nrec.d[11] == 0xFF) memset(((BYTE*)t.data) + 12, 0xFF, 28);
						TameType = nrec.type;
						WildType = pref->type;
					}
					else
					{
						memcpy(w.data, nrec.d, sizeof(nrec.d));
						if (nrec.d[11] == 0xFF) memset(((BYTE*)w.data) + 12, 0xFF, 28);
						memcpy(t.data, pref->d, sizeof(pref->d));
						if (pref->d[11] == 0xFF) memset(((BYTE*)t.data) + 12, 0xFF, 28);
						TameType = TAME;
						WildType = nrec.type;
					}

					bool res = Collision_SOTA(PointToSolve, t, TameType, w, WildType, false) || Collision_SOTA(PointToSolve, t, TameType, w, WildType, true);
					if (!res)
					{
						//bool w12 = ((pref->type == WILD) && (nrec.type == WILD2)) || ((pref->type == WILD2) && (nrec.type == WILD));
						//if (w12) //in rare cases WILD and WILD2 can collide in mirror, in this case there is no way to find K
						//	ToLog("W1 and W2 collided in mirror");
						continue;
					}
					found = true;
					break;
				}
				else
				{
					kangs[i].iter = 0;
					memset(&old[OLD_LEN * i], 0, 8 * OLD_LEN);
				}
			}
		}
#ifdef INTERVAL_STATS
		KToSolve.Sub(x32);
		KToSolve.ShiftRight(RANGE_BITS - INTERVAL_BITS);
		int int_ind = (int)KToSolve.data[0];
		int_cnt[int_ind]++;
		int_sum[int_ind] += pnt_iters;
#endif
		db->Clear(false);
		InterlockedIncrement(&SolvedCnt);
	}
	free(old);
	delete db;
	InterlockedDecrement(&ThrCnt);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#define METHOD_CLASSIC		0
#define METHOD_3WAY			1
#define METHOD_MIRROR		2
#define METHOD_SOTA			3
#define METHOD_SOTA_PLUS	4
char* names[] = { "Classic", "3-Way", "Mirror", "SOTA", "SOTA+" };

void SetSotaParams(int wr, int tr)
{
	x32.Set(1);
	x32.ShiftLeft(RANGE_BITS - 5);
	Pntx32 = ec.MultiplyG(x32);
	WildRange.Set(0);
	for (int i = 0; i < wr; i++)
		WildRange.Add(x32);
	TameRange.Set(0);
	for (int i = 0; i < tr; i++)
		TameRange.Add(x32);
}

void Prepare(int Method)
{
	EcInt minjump, t;
	minjump.Set(1);

	//you can use some complex formula: 
	//int mp2 = (int)(log10(KANG_CNT / 4) / log10(2));
	//minjump.ShiftLeft(RANGE_BITS / 2 + mp2);
	//or some simple formula:
	minjump.ShiftLeft(RANGE_BITS / 2 + 3);
	
	for (int i = 0; i < JMP_CNT; i++)
	{
		EcJumps[i].dist = minjump;
		t.RndMax(minjump);
		EcJumps[i].dist.Add(t);
		if ((Method == METHOD_3WAY) || (Method == METHOD_SOTA) || (Method == METHOD_SOTA_PLUS))
			EcJumps[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
		EcJumps[i].p = ec.MultiplyG(EcJumps[i].dist);	
	}
	Int_HalfRange.Set(1);
	Int_HalfRange.ShiftLeft(RANGE_BITS - 1);
	Pnt_HalfRange = ec.MultiplyG(Int_HalfRange);
	Pnt_NegHalfRange = Pnt_HalfRange;
	Pnt_NegHalfRange.y.NegModP();
	//for 3-way only
	Int_TameOffset.Set(1);
	Int_TameOffset.ShiftLeft(RANGE_BITS - 1);
	EcInt tt;
	tt.Set(1);
	tt.ShiftLeft(RANGE_BITS - 5); //half of tame range width
	Int_TameOffset.Sub(tt);
	Pnt_TameOffset = ec.MultiplyG(Int_TameOffset);

	if ((Method == METHOD_SOTA) || (Method == METHOD_SOTA_PLUS))
		SetSotaParams(32 + 2, 1); //add 2 just to improve K for range edges
}

void TestKangaroo(int Method)
{
	if (ThrCnt)
		return;
	ToLog("Started, please wait...");
#ifdef INTERVAL_STATS
	memset(int_cnt, 0, sizeof(int_cnt));
	memset(int_sum, 0, sizeof(int_sum));
#endif
	SetRndSeed(0);
	Prepare(Method);
	SetRndSeed(GetTickCount64());
	SolvedCnt = 0;
	TThrRec recs[CPU_THR_CNT];
	ThrCnt = CPU_THR_CNT;
	ToSolveCnt = POINTS_CNT;
	u64 tm = GetTickCount64();
	for (int i = 0; i < CPU_THR_CNT; i++)
	{
		u32 ThreadID;
		u32 (__stdcall *thr_proc_ptr)(void*);
		switch (Method)
		{
		case METHOD_CLASSIC:
			thr_proc_ptr = thr_proc_classic;
			break;
		case METHOD_3WAY:
			thr_proc_ptr = thr_proc_3way;
			break;
		case METHOD_MIRROR:
			thr_proc_ptr = thr_proc_mirror;
			break;
		case METHOD_SOTA:
			thr_proc_ptr = thr_proc_sota;
			break;
		case METHOD_SOTA_PLUS:
			thr_proc_ptr = thr_proc_sota_plus;
			break;
		default:
			return;
		}
		recs[i].hThread = (HANDLE)_beginthreadex(NULL, 0, thr_proc_ptr, (void*)&recs[i], 0, &ThreadID);
	}
	char s[300];
	while (ThrCnt)
	{
		sprintf(s, "Threads: %d. Solved: %d of %d", ThrCnt, SolvedCnt, POINTS_CNT);
		dlg->lbTime.SetWindowText(s);
		Sleep(100);
		DoEvents();
	}
	for (int i = 0; i < CPU_THR_CNT; i++)
		CloseHandle(recs[i].hThread);
	tm = GetTickCount64() - tm;
	
	sprintf(s, "Total time: %d sec", (int)(tm/1000));
	ToLog(s);
	size_t iters_sum = recs[0].iters;
	for (int i = 1; i < CPU_THR_CNT; i++)
		iters_sum += recs[i].iters;

	size_t aver = iters_sum / POINTS_CNT;
	sprintf(s, "Average jumps per point: %llu. Average jumps per kangaroo: %llu", aver, aver / KANG_CNT);
	ToLog(s);
	double root = pow(2.0, RANGE_BITS / 2.0);
	double coef = (double)aver / root;
	sprintf(s, "%s, K = %.3f (including DP overhead)", names[Method], coef);
	ToLog(s);
	if (RANGE_BITS < 40)
		ToLog("Note: RANGE_BITS is too small to measure K precisely");
	if (POINTS_CNT < 1000)
		ToLog("Note: POINTS_CNT is too small to measure K precisely");
	dlg->lbTime.SetWindowText("-----");
#ifdef INTERVAL_STATS
	for (int i = 0; i < INTERVAL_CNT; i++)
	{
		if (int_cnt[i])
		{
			double aver_k = (int_sum[i] / int_cnt[i]) / root;
			sprintf(s, "Interval %d: points %d, aver K = %.3f", i, int_cnt[i], aver_k);
			ToLog(s);
		}
	}
#endif
}

void CExpKangDlg::OnBnClickedButton1()
{
	TestKangaroo(METHOD_CLASSIC);
}

void CExpKangDlg::OnBnClickedButton2()
{
	TestKangaroo(METHOD_3WAY);
}

void CExpKangDlg::OnBnClickedButton3()
{
	TestKangaroo(METHOD_MIRROR);
}

void CExpKangDlg::OnBnClickedButton4()
{
	TestKangaroo(METHOD_SOTA);
}

void CExpKangDlg::OnBnClickedButton5()
{
	TestKangaroo(METHOD_SOTA_PLUS);
}
