include "../../../Commands/OptionsBase.td"

let Command = "process plugin scriptable load" in {
  def frame_recognizer_shlib : Option<"shlib", "s">, Arg<"ShlibName">,
    Completion<"Module">,
    Desc<"Name of the Python module that holds the scriptable porcess.">;
  def frame_recognizer_python_class : Option<"python-class", "l">, Group<2>,
    Arg<"PythonClass">,
    Desc<"Name of a Python class that implements the scriptable process.">;
}
