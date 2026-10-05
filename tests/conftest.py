import sys
from pathlib import Path

# The grader's modules run from /grader in the image; import them from source here.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "grader"))
