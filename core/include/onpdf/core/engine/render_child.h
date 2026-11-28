#pragma once

// 렌더 자식 프로세스 본체 (#137). `onpdf_app --render-child <in> <out>` 와 테스트 실행 파일이
// main 첫 줄에서 이 함수로 분기한다(Qt·설정·GUI 초기화 전). 부모의 메시지(render_protocol)를
// 순서대로 처리하고 부모 파이프가 닫히면 끝난다.
//  - 이 프로세스의 PdfiumLibrary 를 첫 Open 때 만든다 (use_system_fonts 는 Open 이 정한다)
//  - 문서는 PdfiumDocument 를 그대로 쓴다 (렌더 위험 캐시·폼 환경 포함 — 부모와 같은 판정)
//  - 환경 변수 ONPDF_RENDER_CHILD_FAULT="crash:<page>" | "hang:<page>" 는 테스트용 장애 주입
//    (그 쪽을 렌더하기 직전에 죽거나 멈춘다)

namespace onpdf::engine {

// argv: [0]=exe, [1]="--render-child", [2]=읽기 핸들, [3]=쓰기 핸들 (platform::Pipe 의 10진 표기).
// 돌려주는 값은 프로세스 종료 코드 (0 정상, 2 인자 오류, 3 파이프 오류)
int run_render_child(int argc, char** argv);

}  // namespace onpdf::engine
