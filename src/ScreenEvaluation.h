#ifndef SCREEN_EVALUATION_H_
#define SCREEN_EVALUATION_H_

#include "Screen.h"
#include <CEGUI.h>

class BitmapFont;

class ScreenEvaluation : public IScreen
{
	EvaluationData Results;
	CEGUI::DefaultWindow* root;
	GraphObject2D Background;
	BitmapFont* Font;

	std::string ResultsString, ResultsNumerical;
	// CEGUI::FrameWindow* wnd;
	int32 CalculateScore();
	bool StopRunning(const CEGUI::EventArgs&);
public:
	ScreenEvaluation(IScreen *Parent);
	void Init(EvaluationData _Data);
	bool Run(double Delta);
	void Cleanup();
	void HandleInput(int32 key, int32 code, bool isMouseInput);
};

#endif