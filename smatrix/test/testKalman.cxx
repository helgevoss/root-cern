

#include "Math/SVector.h"
#include "Math/SMatrix.h"

#include "TMatrixD.h"
#include "TVectorD.h"

#include "TRandom3.h"

// #include "SealUtil/SealTimer.h"
// #include "SealUtil/SealHRRTChrono.h"
// #include "SealUtil/TimingReport.h"

#include <iostream>

#ifndef NDIM1
#define NDIM1 2
#endif
#ifndef NDIM2
#define NDIM2 5
#endif

#define NITER 1  // number of iterations

#define NLOOP 1000000 // number of time the test is repeted

using namespace ROOT::Math;

#include "TestTimer.h"

int test_smatrix_kalman() {
    
  // need to write explicitly the dimensions
   

  typedef SMatrix<double, NDIM1, NDIM1>  MnMatrixNN;
  typedef SMatrix<double, NDIM2, NDIM2>  MnMatrixMM;
  typedef SMatrix<double, NDIM1, NDIM2>  MnMatrixNM;
  typedef SMatrix<double, NDIM2 , NDIM1> MnMatrixMN;
  typedef SMatrix<double, NDIM1 >        MnSymMatrixNN;
  typedef SMatrix<double, NDIM2 >        MnSymMatrixMM;
  typedef SVector<double, NDIM1>         MnVectorN;
  typedef SVector<double, NDIM2>         MnVectorM;
  


  int first = NDIM1;  //Can change the size of the matrices
  int second = NDIM2;
  

  std::cout << "************************************************\n";
  std::cout << "  SMatrix kalman test  "   <<  first << " x " << second  << std::endl;
  std::cout << "************************************************\n";

  
  
   
  int npass = NITER; 
  TRandom3 r(111);
  double x2sum = 0, c2sum = 0;
  for (int k = 0; k < npass; k++) {



    MnMatrixNM H;
    MnMatrixMN K0;
    MnSymMatrixMM Cp; 
    MnSymMatrixNN V; 
    MnVectorN m;
    MnVectorM xp;


    { 

      // fill matrices with ranodm data
      for(int i = 0; i < first; i++) 
	for(int j = 0; j < second; j++)
	  H(i,j) = r.Rndm() + 1.;
      for(int i = 0; i < second; i++) 
	for(int j = 0; j < first; j++) 
	  K0(i,j) = r.Rndm() + 1.;
      // sym matrices 
      for(int i = 0; i < second; i++) 
	for(int j = 0; j <=i; j++) {  
	  Cp(i,j) = r.Rndm() + 1.;
	  if (j != i) Cp(j,i) = Cp(i,j);
	}
      for(int i = 0; i < first; i++) 
	for(int j = 0; j <=i; j++) {
	  V(i,j) = r.Rndm() + 1.;
	  if (j != i) V(j,i) = V(i,j);
	}
      // vectors
      for(int i = 0; i < first; i++) 
	m(i) = r.Rndm() + 1.;
      for(int i = 0; i < second; i++) 
	xp(i) = r.Rndm() + 1.;

    }


    MnSymMatrixMM I; 
    for(int i = 0; i < second; i++) 
      I(i,i) = 1;
     
#ifdef DEBUG
    std::cout << "pass " << k << std::endl;
    if (k == 0) { 
      std::cout << " K0 = " << K0 << std::endl;
      std::cout << " H = " << K0 << std::endl;
      std::cout << " Cp = " << Cp << std::endl;
      std::cout << " V = " << V << std::endl;
      std::cout << " m = " << m << std::endl;
      std::cout << " xp = " << xp << std::endl;
    }
#endif
	
    
    {
      double x2 = 0,c2 = 0;
      test::Timer t("SMatrix Kalman ");

      MnVectorM x; 
      MnMatrixMN tmp;   
      MnSymMatrixNN Rinv; 
      MnMatrixMN K; 
      MnSymMatrixMM C; 
      //MnMatrixNM tmp2; 
      MnMatrixMM tmp2; 
      MnVectorN vtmp1; 
      MnVectorN vtmp; 

      for (int l = 0; l < NLOOP; l++) 	
	{



	  vtmp1 = H*xp -m;
	  //x = xp + K0 * (m- H * xp);
	  x = xp - K0 * vtmp1;
	  tmp = Cp * Transpose(H);
	  Rinv = V;  Rinv +=  H * tmp;

	  bool test = Rinv.Invert();
 	  if(!test) { 
 	    std::cout<<"inversion failed" <<std::endl;
	    std::cout << Rinv << std::endl;
	  }

	  K =  tmp * Rinv ; 
	  tmp2 = K * H;
	  C = Cp - tmp2 * Cp;
	  //C = ( I - K * H ) * Cp;
	  //x2 = Product(Rinv,m-H*xp);  // this does not compile on WIN32
 	  vtmp = m-H*xp; 
 	  x2 = Dot(vtmp, Rinv*vtmp);

	}
	//std::cout << k << " chi2 = " << x2 << std::endl;
      x2sum += x2;
      c2 = 0;
      for (int i=0; i<NDIM2; ++i)
	for (int j=0; j<NDIM2; ++j)
	  c2 += C(i,j);
      c2sum += c2;
    }
  }
  //tr.dump();

  std::cout << "x2sum = " << x2sum << "\tc2sum = " << c2sum << std::endl;

  return 0;
}

// ROOT test 


int test_tmatrix_kalman() {


    

  typedef TMatrixD MnMatrix;
  typedef TVectorD MnVector;
  
//   typedef boost::numeric::ublas::matrix<double>  MnMatrix;  
  //typedef HepSymMatrix MnSymMatrixHep; 


  int first = NDIM1;  //Can change the size of the matrices
  int second = NDIM2;


  std::cout << "************************************************\n";
  std::cout << "  TMatrix Kalman test  "   <<  first << " x " << second  << std::endl;
  std::cout << "************************************************\n";
  
  
   
  int npass = NITER; 
  TRandom3 r(111);
  double x2sum = 0,c2sum = 0;

  for (int k = 0; k < npass; k++) 
  {

      MnMatrix H(first,second);
      for(int i = 0; i < first; i++)
	for(int j = 0; j < second; j++)
	  H(i,j) = r.Rndm() + 1.;
      MnMatrix K0(second,first);
      for(int i = 0; i < second; i++)
	for(int j = 0; j < first; j++)
	  K0(i,j) = r.Rndm() + 1.;
      //MnSymMatrix Cp(second);
      MnMatrix Cp(second,second);
      //in ROOT a sym matrix is like a normal matrix
      for(int i = 0; i < second; i++)
	for(int j = 0; j <=i; j++){ 
	  Cp(i,j) = r.Rndm() + 1.;
	  if (j != i) Cp(j,i) = Cp(i,j);
	}
      //MnSymMatrix V(first);
      MnMatrix V(first,first);
      for(int i = 0; i < first; i++)
	for(int j = 0; j <=i; j++) { 
	  V(i,j) = r.Rndm() + 1.;
	  if (j != i) V(j,i) = V(i,j);
	}
      MnVector m(first);
      for(int j = 0; j < first; j++)
	m(j) = r.Rndm() + 1.;
      MnVector xp(second);
      for(int j = 0; j < second; j++)
	xp(j) = r.Rndm() + 1.;
      //MnSymMatrix I(second);//Identity matrix
      MnMatrix I(second,second);//Identity matrix
      for (int i = 0; i < second; i++)
	for(int j = 0; j <second; j++) { 
	  I(i,j) = 0.0; 
	  if (i==j) I(i,i) = 1.0;
	}

//       if (k==0) { 
// 	std::cout << " Cp " << std::endl;
// 	Cp.Print();
//       }

    {
      double x2 = 0,c2 = 0;
      TVectorD x(second);
      TMatrixD Rinv(first,first);
      TMatrixDSym RinvSym;
      TMatrixD K(second,first);
      TMatrixD C(second,second);
      TVectorD tmp1(first);
      TMatrixD tmp2(second,first);
      
      test::Timer t("TMatrix Kalman ");
      for (Int_t l = 0; l < NLOOP; l++)
      {
        tmp1 = m; Add(tmp1,-1.0,H,xp);
        x = xp; Add(x,+1.0,K0,tmp1);
        tmp2 = TMatrixD(Cp,TMatrixD::kMultTranspose,H);
        Rinv = V ; Rinv += TMatrixD(H,TMatrixD::kMult,tmp2);
        RinvSym.Use(first,Rinv.GetMatrixArray()); RinvSym.InvertFast();
        C = Cp; C -= TMatrixD(TMatrixD(tmp2,TMatrixD::kMult,Rinv),TMatrixD::kMult,TMatrixD(H,TMatrixD::kMult,Cp));
        x2 = RinvSym.Similarity(tmp1);

      }
      x2sum += x2; 
      c2 = 0;
      for (int i=0; i<NDIM2; ++i)
	for (int j=0; j<NDIM2; ++j)
	  c2 += C(i,j);
      c2sum += c2;
    }

      //   }
  }  
  //tr.dump();
  std::cout << "x2sum = " << x2sum << "\tc2sum = " << c2sum << std::endl;
  
  return 0;
}



int main() { 

  test_smatrix_kalman();
  test_tmatrix_kalman();
}
