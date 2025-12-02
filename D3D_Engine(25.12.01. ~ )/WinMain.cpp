// 01_imgui.cpp : 애플리케이션에 대한 진입점을 정의합니다.
//

#include "../D3D_Core/pch.h"
#include "TutorialApp.h"


int APIENTRY wWinMain( // wWinMain << 유니코드(UTF-16) 버전
	//APIENTRY << api entry // 뭔가 규칙같은걸 선언하는거임(잘 몰루겟음)
	_In_ HINSTANCE hInstance, // 현재 모듈(프로세스)의 인스턴스 핸들, 창 클래스 등록이나 리소스 로딩등에서 넘겨짐
	// _In_ , In_opt_ 이런거는 정적 분석용 힌트임, 해당 매개변수의 성질을 나타냄
	// In<< 필수 입력 / In_opt << 선택 입력 / Out << 필수 출력 이런느낌
	_In_opt_ HINSTANCE hPrevInstance, // 필수 요소는 아님, 모르겠음 그냥 흔적기관임(안씀)
	_In_ LPWSTR    lpCmdLine, // 프로그램 실행시 명령줄을 읽는다는데, 명령줄이 뭐지
	// mygame.exe -level 3 -debug << 이런거인듯, 이름(mygame.exe) 뒤에 오는 명령줄을 여기에 넣는거임
	// 마찬가지로 In이 붙은걸로 보아, 유효한 값이 들어가야함
	_In_ int       nCmdShow) // 초기 창 상태를 표시한다고 함, 최소화 최대화? 그런거
{
	TutorialApp App;
	return App.Run(hInstance);
}
