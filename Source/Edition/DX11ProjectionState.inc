// SPDX-License-Identifier: GPL-3.0-or-later
// Inside CDX11VideoProcessor projection/color state. Keep declaration order; this fragment is not a standalone header.
	bool m_jvrFlatView = false;
	int m_jvrEye = 0;
	jvr::ViewTransform m_jvrView;
	std::array<float, 16> m_jvrUploadedConstants{};
	struct SvdLutScene {
		REFERENCE_TIME start;
		bool submitted = false;
		bool fadeIn = false;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
	};
	struct {
		svdlut::ModelWorker model;
		std::unique_ptr<svdlut::Image> input;
		std::deque<SvdLutScene> scenes;
		std::array<float, 8 * 8 * 3> signature{};
		std::array<float, 8 * 8 * 3> cutSignature{};
		REFERENCE_TIME captured = INVALID_TIME;
		REFERENCE_TIME possibleCut = INVALID_TIME;
		REFERENCE_TIME inference = INVALID_TIME;
		REFERENCE_TIME transitionStart = INVALID_TIME;
		REFERENCE_TIME nextCapture = INVALID_TIME;
		int strength = 0;
		bool attempted = false;
		bool failed = false;
		bool hasSignature = false;
		bool active = false;
		bool waitingForBuffer = false;
		bool clearViewSupported = false;
		Tex2D_t picture, nativeThumbnail, thumbnail, staging;
		CComPtr<ID3D11SamplerState> lutSampler;
		CComPtr<ID3D11Buffer> constants;
	} m_svdLut;

	bool m_jvrShaderFailed = false;
	Tex2D_t m_jvrSource;
	Tex2D_t m_jvrFrame;
	CComPtr<ID3D11PixelShader> m_jvrShader;
	CComPtr<ID3D11Buffer> m_jvrConstants;

